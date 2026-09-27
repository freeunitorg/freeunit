/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package nginx.unit.websocket.server;

import java.io.EOFException;
import java.io.IOException;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.channels.CompletionHandler;
import java.nio.channels.InterruptedByTimeoutException;
import java.util.ArrayDeque;
import java.util.Queue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;

import javax.websocket.SendHandler;
import javax.websocket.SendResult;

import org.apache.juli.logging.Log;
import org.apache.juli.logging.LogFactory;
import org.apache.tomcat.util.res.StringManager;
import nginx.unit.websocket.Transformation;
import nginx.unit.websocket.WsRemoteEndpointImplBase;

/**
 * This is the server side {@link javax.websocket.RemoteEndpoint} implementation
 * - i.e. what the server uses to send data to the client.
 */
public class WsRemoteEndpointImplServer extends WsRemoteEndpointImplBase {

    private static final StringManager sm =
            StringManager.getManager(WsRemoteEndpointImplServer.class);
    private final Log log = LogFactory.getLog(WsRemoteEndpointImplServer.class); // must not be static

    private volatile SendHandler handler = null;
    private volatile ByteBuffer[] buffers = null;

    private volatile long timeoutExpiry = -1;
    private volatile boolean close;

    /*
     * A frame that the batching path split across two doWrite() calls: the
     * base class fills its output buffer and flushes it wherever it happens
     * to be full, so a frame's header, or its payload, may arrive in parts.
     * A server frame is not masked, so its header is at most 10 bytes.
     */
    private final byte[] header = new byte[10];
    private int headerLength = 0;
    private byte frameOpCode;
    private boolean frameFin;
    private long frameLeft = 0;
    private ByteBuffer framePayload = null;

    /*
     * A completion runs on the thread that sent.  A handler that sends from
     * onResult(), as TextMessageSendHandler does for each 8 KiB of a text
     * message, nests a doWrite() inside the completion, so a large message
     * would nest thousands deep.  Completions nest inline up to MAX_DEPTH and
     * are queued beyond it, to be run by the outermost completion on this
     * thread.  Inline, not always queued: a handler that waits on a send it
     * made itself (Future.get(), flushBatch(), close() with batching on) needs
     * that send's completion to run before it returns.
     *
     * The state is per thread: once a completion releases the message part,
     * another thread may send on this endpoint while the first is still
     * inside its completion.
     */
    private static final int MAX_DEPTH = 32;

    private static final ThreadLocal<Completions> completions =
            new ThreadLocal<>();

    public WsRemoteEndpointImplServer(
            WsServerContainer serverContainer) {
    }


    @Override
    protected final boolean isMasked() {
        return false;
    }

    /*
     * Sends the frames the base class serialised into buffers.  Unit writes
     * the frame header itself, so each frame is decoded back into its opcode,
     * fin bit and payload and handed to sendWsFrame().
     *
     * writeMessagePart() sends a frame directly as its header and its payload,
     * in that order.  With batching allowed it copies frames into its output
     * buffer instead and passes that buffer whenever it fills or is flushed,
     * so that path is decoded as a byte stream that may split a frame across
     * calls.
     *
     * The write is synchronous: sendWsFrame() has copied the payload to the
     * router by the time it returns, so the handler is completed here, on the
     * calling thread, with the outcome.
     */
    @Override
    protected void doWrite(SendHandler handler, long blockingWriteTimeoutExpiry,
            ByteBuffer... buffers) {
        SendResult result = SENDRESULT_OK;

        try {
            if (!isSessionOpen()) {
                throw new IOException(sm.getString("wsRemoteEndpointServer.closed"));
            }

            if (buffers.length == 2 && headerLength == 0 && frameLeft == 0) {
                // Direct path: the frame's header, then its payload.
                ByteBuffer hdr = buffers[0];
                ByteBuffer payload = buffers[1];
                byte b = hdr.get(hdr.position());

                sendWsFrame(payload, (byte) (b & 0x0F), (b & 0x80) != 0,
                        blockingWriteTimeoutExpiry);

                hdr.position(hdr.limit());
                payload.position(payload.limit());
            } else {
                for (ByteBuffer buffer : buffers) {
                    sendFrames(buffer, blockingWriteTimeoutExpiry);
                }
            }
        } catch (IOException | RuntimeException e) {
            /*
             * A frame the stream decoder had started is lost with the send;
             * start the next doWrite() at a frame boundary.
             */
            headerLength = 0;
            frameLeft = 0;
            framePayload = null;

            result = new SendResult(e);
        }

        complete(handler, result);
    }


    private void sendFrames(ByteBuffer buffer, long timeoutExpiry)
            throws IOException {
        while (buffer.hasRemaining()) {
            if (frameLeft == 0 && framePayload == null) {
                if (!readHeader(buffer)) {
                    // The rest of the header is in a later buffer
                    return;
                }
                if (frameLeft == 0) {
                    sendWsFrame(ByteBuffer.allocate(0), frameOpCode, frameFin,
                            timeoutExpiry);
                    continue;
                }
            }

            int limit = buffer.limit();

            if (framePayload == null && buffer.remaining() >= frameLeft) {
                // The whole payload is here: send it in place
                buffer.limit(buffer.position() + (int) frameLeft);
                sendWsFrame(buffer, frameOpCode, frameFin, timeoutExpiry);
                buffer.position(buffer.limit());
                buffer.limit(limit);
                frameLeft = 0;
                continue;
            }

            // The payload is split across buffers: gather it
            if (framePayload == null) {
                framePayload = ByteBuffer.allocate((int) frameLeft);
            }

            int n = Math.min(buffer.remaining(), framePayload.remaining());
            buffer.limit(buffer.position() + n);
            framePayload.put(buffer);
            buffer.limit(limit);
            frameLeft -= n;

            if (frameLeft == 0) {
                framePayload.flip();
                ByteBuffer payload = framePayload;
                framePayload = null;
                sendWsFrame(payload, frameOpCode, frameFin, timeoutExpiry);
            }
        }
    }


    /*
     * Reads header bytes from buffer until the header is complete.  Returns
     * false when buffer ran out first; the bytes read so far are kept for the
     * next call.
     */
    private boolean readHeader(ByteBuffer buffer) throws IOException {
        while (buffer.hasRemaining()) {
            header[headerLength++] = buffer.get();

            if (headerLength < 2) {
                continue;
            }

            int len = header[1] & 0x7F;
            int need = len == 126 ? 4 : len == 127 ? 10 : 2;

            if (headerLength < need) {
                continue;
            }

            frameFin = (header[0] & 0x80) != 0;
            frameOpCode = (byte) (header[0] & 0x0F);

            if (len == 126) {
                frameLeft = ((header[2] & 0xFF) << 8) | (header[3] & 0xFF);
            } else if (len == 127) {
                frameLeft = 0;
                for (int i = 2; i < 10; i++) {
                    frameLeft = (frameLeft << 8) | (header[i] & 0xFF);
                }
                if (frameLeft < 0 || frameLeft > Integer.MAX_VALUE) {
                    headerLength = 0;
                    throw new IOException(sm.getString(
                            "wsRemoteEndpointServer.badFrame",
                            Long.valueOf(frameLeft)));
                }
            } else {
                frameLeft = len;
            }

            headerLength = 0;
            return true;
        }

        return false;
    }


    private void complete(SendHandler handler, SendResult result) {
        Completions c = completions.get();

        if (c == null) {
            c = new Completions();
            completions.set(c);

        } else if (c.depth >= MAX_DEPTH) {
            c.queue.add(new Completion(handler, result));
            return;
        }

        boolean outermost = c.depth == 0;

        c.depth++;
        try {
            handler.onResult(result);
        } finally {
            c.depth--;

            if (outermost) {
                drain(c);
            }
        }
    }


    private static void drain(Completions c) {
        try {
            Completion q;
            while ((q = c.queue.poll()) != null) {
                c.depth++;
                try {
                    q.handler.onResult(q.result);
                } finally {
                    c.depth--;
                }
            }
        } finally {
            completions.remove();
        }
    }


    private static class Completions {
        private int depth = 0;
        private final Queue<Completion> queue = new ArrayDeque<>();
    }


    private static class Completion {
        private final SendHandler handler;
        private final SendResult result;

        private Completion(SendHandler handler, SendResult result) {
            this.handler = handler;
            this.result = result;
        }
    }

    @Override
    protected void doClose() {
        if (handler != null) {
            // close() can be triggered by a wide range of scenarios. It is far
            // simpler just to always use a dispatch than it is to try and track
            // whether or not this method was called by the same thread that
            // triggered the write
            clearHandler(new EOFException(), true);
        }
    }


    protected long getTimeoutExpiry() {
        return timeoutExpiry;
    }


    /*
     * Currently this is only called from the background thread so we could just
     * call clearHandler() with useDispatch == false but the method parameter
     * was added in case other callers started to use this method to make sure
     * that those callers think through what the correct value of useDispatch is
     * for them.
     */
    protected void onTimeout(boolean useDispatch) {
        if (handler != null) {
            clearHandler(new SocketTimeoutException(), useDispatch);
        }
        close();
    }


    @Override
    protected void setTransformation(Transformation transformation) {
        // Overridden purely so it is visible to other classes in this package
        super.setTransformation(transformation);
    }


    /**
     *
     * @param t             The throwable associated with any error that
     *                      occurred
     * @param useDispatch   Should {@link SendHandler#onResult(SendResult)} be
     *                      called from a new thread, keeping in mind the
     *                      requirements of
     *                      {@link javax.websocket.RemoteEndpoint.Async}
     */
    private void clearHandler(Throwable t, boolean useDispatch) {
        // Setting the result marks this (partial) message as
        // complete which means the next one may be sent which
        // could update the value of the handler. Therefore, keep a
        // local copy before signalling the end of the (partial)
        // message.
        SendHandler sh = handler;
        handler = null;
        buffers = null;
        if (sh != null) {
            if (useDispatch) {
                OnResultRunnable r = new OnResultRunnable(sh, t);
            } else {
                if (t == null) {
                    sh.onResult(new SendResult());
                } else {
                    sh.onResult(new SendResult(t));
                }
            }
        }
    }


    private static class OnResultRunnable implements Runnable {

        private final SendHandler sh;
        private final Throwable t;

        private OnResultRunnable(SendHandler sh, Throwable t) {
            this.sh = sh;
            this.t = t;
        }

        @Override
        public void run() {
            if (t == null) {
                sh.onResult(new SendResult());
            } else {
                sh.onResult(new SendResult(t));
            }
        }
    }
}
