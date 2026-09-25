import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

import javax.websocket.OnMessage;
import javax.websocket.OnOpen;
import javax.websocket.RemoteEndpoint;
import javax.websocket.SendHandler;
import javax.websocket.SendResult;
import javax.websocket.Session;
import javax.websocket.server.ServerEndpoint;

/*
 * A mirror app that echoes through the asynchronous remote (issue #434).
 *
 * A text message that starts with "handler:" is echoed with sendText(String,
 * SendHandler); one that starts with "object:" with sendObject(Object), as an
 * Integer when the rest of it is one and as a String otherwise; any other
 * text message with the Future from sendText(String).  A binary message
 * whose first byte is 'h' is echoed with sendBinary(ByteBuffer, SendHandler),
 * any other with the Future from sendBinary(ByteBuffer).
 *
 * After the echo the app reports what the asynchronous API told it, as a text
 * frame sent through the blocking remote: "handler-ok" or "handler-fail: ..."
 * once the SendHandler has run, "future-done" once Future.get() has returned,
 * "future-timeout" when it has not returned within five seconds, or
 * "future-fail: ..." when it threw.  So a client sees exactly one of the two
 * defects of #434 when the echo is missing, and the other when the report is.
 *
 * "batch:N" allows batching, sends N messages "b0".."b<N-1>" through the
 * asynchronous remote, then disallows batching again, which flushes them, and
 * reports "batch-flushed" or "batch-fail: ...".  "threads:N" sends "t0".."t<N-1>"
 * from N threads at once and reports "threads-done" once every future has
 * completed, or the first failure or timeout.  A binary message whose first
 * byte is 'o' is sent batched from its fourth byte on; see offset().
 * "nested:..." waits on sends from inside a SendHandler; see nested().
 */
@ServerEndpoint("/")
public class app {

    @OnOpen
    public void onOpen(Session session) {
        session.setMaxTextMessageBufferSize(16 * 1024 * 1024);
        session.setMaxBinaryMessageBufferSize(16 * 1024 * 1024);
    }

    @OnMessage
    public void echoTextMessage(Session session, String msg) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();

        if (msg.startsWith("nested:")) {
            nested(session, msg);
        } else if (msg.startsWith("batch:")) {
            batch(session, Integer.parseInt(msg.substring("batch:".length())));
        } else if (msg.startsWith("threads:")) {
            threads(session,
                    Integer.parseInt(msg.substring("threads:".length())));
        } else if (msg.startsWith("handler:")) {
            remote.sendText(msg, new Reporter(session));
        } else if (msg.startsWith("object:")) {
            /*
             * An Integer takes sendObject()'s primitive path and is echoed as
             * its decimal string; anything else is sent as a String, which
             * has no encoder, so the future must complete with a failure.
             * That failure is this fork's behaviour (Util.isPrimitive() does
             * not count String), not something the specification requires:
             * the case is here to show a failed future completes.
             */
            String arg = msg.substring("object:".length());
            Object obj;
            try {
                obj = Integer.valueOf(arg);
            } catch (NumberFormatException e) {
                obj = arg;
            }
            report(session, waitFor(remote.sendObject(obj)));
        } else {
            report(session, waitFor(remote.sendText(msg)));
        }
    }

    @OnMessage
    public void echoBinaryMessage(Session session, ByteBuffer bb) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();

        if (bb.remaining() > 3 && bb.get(bb.position()) == 'o') {
            offset(session, bb);
        } else if (bb.remaining() > 0 && bb.get(bb.position()) == 'h') {
            remote.sendBinary(bb, new Reporter(session));
        } else {
            report(session, waitFor(remote.sendBinary(bb)));
        }
    }

    private static void batch(Session session, int n) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();

        try {
            remote.setBatchingAllowed(true);

            for (int i = 0; i < n; i++) {
                String status = waitFor(remote.sendText("b" + i));
                if (!status.equals("future-done")) {
                    report(session, "batch-fail: " + status);
                    return;
                }
            }

            remote.setBatchingAllowed(false);
        } catch (Exception e) {
            report(session, "batch-fail: " + e);
            return;
        }

        report(session, "batch-flushed");
    }

    /*
     * With batching allowed, sends the message from its fourth byte on, as a
     * buffer whose position() is not 0, then a text frame "after", flushes,
     * and reports "offset-flushed".  The header of the first frame has to
     * give its real length, or the second one is read as part of it.
     */
    private static void offset(Session session, ByteBuffer bb) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();

        try {
            remote.setBatchingAllowed(true);

            ByteBuffer copy = ByteBuffer.allocate(bb.remaining());
            copy.put(bb);
            copy.flip();
            copy.position(3);

            String status = waitFor(remote.sendBinary(copy));
            if (status.equals("future-done")) {
                status = waitFor(remote.sendText("after"));
            }
            if (!status.equals("future-done")) {
                report(session, "offset-fail: " + status);
                return;
            }

            remote.setBatchingAllowed(false);
        } catch (Exception e) {
            report(session, "offset-fail: " + e);
            return;
        }

        report(session, "offset-flushed");
    }

    /*
     * Echoes msg with a SendHandler whose onResult() waits on a send of its
     * own, "nested-b", then batches "nested-c" and flushes it, and reports
     * "nested-done", or the first failure.  Both waits need the nested
     * send's completion to run before onResult() returns.
     */
    private static void nested(Session session, String msg) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();

        remote.sendText(msg, result -> {
            String status = waitFor(remote.sendText("nested-b"));
            if (!status.equals("future-done")) {
                report(session, "nested-fail: " + status);
                return;
            }

            try {
                remote.setBatchingAllowed(true);
                status = waitFor(remote.sendText("nested-c"));
                remote.setBatchingAllowed(false);
            } catch (Exception e) {
                report(session, "nested-fail: " + e);
                return;
            }

            report(session, status.equals("future-done")
                            ? "nested-done" : "nested-fail: " + status);
        });
    }

    private static void threads(Session session, int n) {
        RemoteEndpoint.Async remote = session.getAsyncRemote();
        List<Thread> threads = new ArrayList<>();
        String[] status = new String[n];

        for (int i = 0; i < n; i++) {
            final int k = i;
            Thread t = new Thread(() -> {
                /*
                 * A second send on the endpoint while the first is in flight
                 * is refused by the specification, so retry until this one
                 * gets its turn; what is under test is that every accepted
                 * send completes.
                 */
                for (;;) {
                    try {
                        status[k] = waitFor(remote.sendText("t" + k));
                        return;
                    } catch (IllegalStateException e) {
                        Thread.yield();
                    }
                }
            });
            threads.add(t);
            t.start();
        }

        for (int i = 0; i < n; i++) {
            try {
                threads.get(i).join(10000);
            } catch (InterruptedException e) {
                report(session, "threads-fail: " + e);
                return;
            }
            if (!"future-done".equals(status[i])) {
                report(session, "threads-fail: " + i + " " + status[i]);
                return;
            }
        }

        report(session, "threads-done");
    }

    private static String waitFor(Future<Void> future) {
        try {
            future.get(5, TimeUnit.SECONDS);
        } catch (java.util.concurrent.TimeoutException e) {
            return "future-timeout";
        } catch (Exception e) {
            return "future-fail: " + e;
        }
        return "future-done";
    }

    private static void report(Session session, String status) {
        try {
            session.getBasicRemote().sendText(status);
        } catch (IOException e) {
            // Ignore
        }
    }

    private static class Reporter implements SendHandler {
        private final Session session;

        Reporter(Session session) {
            this.session = session;
        }

        @Override
        public void onResult(SendResult result) {
            if (result.isOK()) {
                report(session, "handler-ok");
            } else {
                report(session, "handler-fail: " + result.getException());
            }
        }
    }
}
