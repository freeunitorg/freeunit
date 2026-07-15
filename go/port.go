/*
 * Copyright (C) Max Romanov
 * Copyright (C) NGINX, Inc.
 */

package unit

/*
#include "nxt_cgo_lib.h"
*/
import "C"

import (
	"io"
	"net"
	"os"
	"sync"
	"syscall"
	"unsafe"
)

type port_key struct {
	pid int
	id  int
}

type port struct {
	key port_key
	rcv *net.UnixConn
	snd *net.UnixConn
}

type port_registry struct {
	sync.RWMutex
	m map[port_key]*port
}

var port_registry_ port_registry

func find_port(key port_key) *port {
	port_registry_.RLock()
	res := port_registry_.m[key]
	port_registry_.RUnlock()

	return res
}

// add_port registers p and reports whether it was inserted.  A duplicate key
// (libunit re-announced an existing port) leaves p unregistered, so the caller
// must close p to release its dup'd descriptors.
func add_port(p *port) bool {

	port_registry_.Lock()
	if port_registry_.m == nil {
		port_registry_.m = make(map[port_key]*port)
	}

	old := port_registry_.m[p.key]

	inserted := old == nil

	if inserted {
		port_registry_.m[p.key] = p
	}

	port_registry_.Unlock()

	return inserted
}

func (p *port) Close() {
	if p.rcv != nil {
		p.rcv.Close()
	}

	if p.snd != nil {
		p.snd.Close()
	}
}

func getUnixConn(fd int) *net.UnixConn {
	if fd < 0 {
		return nil
	}

	// Duplicate the descriptor so that libunit stays the sole owner of the
	// original fd number stored in the port struct.  net.FileConn() below
	// dups again (close-on-exec) for its own use, and the "defer f.Close()"
	// then closes only this dup, never the caller's fd.  Closing the original
	// here (the old behaviour) raced with libunit's own close on port
	// destruction, causing "close(N) failed: Bad file descriptor" alerts or,
	// worse, closing an already reused live descriptor.
	//
	// The dup must be close-on-exec: plain dup(2) clears FD_CLOEXEC, so a Go
	// app that fork/exec's while this runs could leak the Unit port socket
	// into a child and keep the port alive.  Hold ForkLock across dup +
	// CloseOnExec (the Go stdlib idiom for non-atomic O_CLOEXEC dups) so no
	// concurrent forkExec observes the fd before its flag is set.
	syscall.ForkLock.RLock()
	newfd, err := syscall.Dup(fd)
	if err != nil {
		syscall.ForkLock.RUnlock()
		nxt_go_alert("dup(%d) failed: %s", fd, err)
		return nil
	}
	syscall.CloseOnExec(newfd)
	syscall.ForkLock.RUnlock()

	f := os.NewFile(uintptr(newfd), "sock")
	defer f.Close()

	c, err := net.FileConn(f)
	if err != nil {
		nxt_go_alert("FileConn error %s", err)
		return nil
	}

	uc, ok := c.(*net.UnixConn)
	if !ok {
		nxt_go_alert("Not a Unix-domain socket %d", fd)
		return nil
	}

	return uc
}

//export nxt_go_add_port
func nxt_go_add_port(ctx *C.nxt_unit_ctx_t, p *C.nxt_unit_port_t) C.int {

	new_port := &port{
		key: port_key{
			pid: int(p.id.pid),
			id:  int(p.id.id),
		},
		rcv: getUnixConn(int(p.in_fd)),
		snd: getUnixConn(int(p.out_fd)),
	}

	// A present fd that failed to wrap (dup/FileConn error) would register a
	// port with a nil rcv/snd and panic later in nxt_go_port_send/recv.  Close
	// any partial dups and fail the callback instead; libunit still owns and
	// closes the original descriptors.
	if (p.in_fd >= 0 && new_port.rcv == nil) ||
		(p.out_fd >= 0 && new_port.snd == nil) {
		new_port.Close()
		return C.NXT_UNIT_ERROR
	}

	if !add_port(new_port) {
		// Duplicate key: libunit re-announced an existing port, so new_port
		// was not registered.  Close its dups instead of leaking them to GC;
		// libunit still owns and closes the original descriptors.
		new_port.Close()
	}

	// Do NOT clear p.in_fd/p.out_fd here.  getUnixConn() now works on private
	// dups, so the original descriptors remain owned by libunit, which closes
	// them exactly once when the port is destroyed.  Go holds independent dups
	// for its own socket I/O.

	return C.NXT_UNIT_OK
}

//export nxt_go_ready
func nxt_go_ready(ctx *C.nxt_unit_ctx_t) C.int {
	go func(ctx *C.nxt_unit_ctx_t) {
		C.nxt_unit_run_shared(ctx)
	}(ctx)

	return C.NXT_UNIT_OK
}

//export nxt_go_remove_port
func nxt_go_remove_port(unit *C.nxt_unit_t, ctx *C.nxt_unit_ctx_t,
	p *C.nxt_unit_port_t) {

	key := port_key{
		pid: int(p.id.pid),
		id:  int(p.id.id),
	}

	port_registry_.Lock()
	if port_registry_.m != nil {
		delete(port_registry_.m, key)
	}

	port_registry_.Unlock()
}

//export nxt_go_port_send
func nxt_go_port_send(pid C.int, id C.int, buf unsafe.Pointer, buf_size C.int,
	oob unsafe.Pointer, oob_size C.int) C.ssize_t {

	key := port_key{
		pid: int(pid),
		id:  int(id),
	}

	p := find_port(key)

	if p == nil {
		nxt_go_alert("port %d:%d not found", pid, id)
		return 0
	}

	n, oobn, err := p.snd.WriteMsgUnix(GoBytes(buf, buf_size),
		GoBytes(oob, oob_size), nil)

	if err != nil {
		nxt_go_warn("write result %d (%d), %s", n, oobn, err)

		n = -1
	}

	return C.ssize_t(n)
}

//export nxt_go_port_recv
func nxt_go_port_recv(pid C.int, id C.int, buf unsafe.Pointer, buf_size C.int,
	oob unsafe.Pointer, oob_size *C.size_t) C.ssize_t {

	key := port_key{
		pid: int(pid),
		id:  int(id),
	}

	p := find_port(key)

	if p == nil {
		nxt_go_alert("port %d:%d not found", pid, id)
		return 0
	}

	n, oobn, _, _, err := p.rcv.ReadMsgUnix(GoBytes(buf, buf_size),
		GoBytes(oob, C.int(*oob_size)))

	if err != nil {
		if nerr, ok := err.(*net.OpError); ok {
			if nerr.Err == io.EOF {
				return 0
			}
		}

		nxt_go_warn("read result %d (%d), %s", n, oobn, err)

		n = -1

	} else {
		*oob_size = C.size_t(oobn)
	}

	return C.ssize_t(n)
}
