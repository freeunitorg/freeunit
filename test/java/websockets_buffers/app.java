import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

import javax.websocket.OnMessage;
import javax.websocket.Session;
import javax.websocket.server.ServerEndpoint;

/*
 * Sends the bytes "456789ab" from a heap buffer that does not start at its
 * array's first byte (issue #486): a slice for "<remote>:slice", a read-only
 * buffer for "<remote>:readonly", through the async remote when <remote> is
 * "async" and the blocking one otherwise.  Then reports "done" or
 * "fail: ...".
 */
@ServerEndpoint("/")
public class app {

    private static final byte[] DATA =
        "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);

    @OnMessage
    public void onMessage(Session session, String msg) throws IOException {
        ByteBuffer bb = ByteBuffer.wrap(DATA, 4, 8);
        bb = msg.endsWith(":slice") ? bb.slice() : bb.asReadOnlyBuffer();

        try {
            if (msg.startsWith("async:")) {
                session.getAsyncRemote().sendBinary(bb).get(5, TimeUnit.SECONDS);
            } else {
                session.getBasicRemote().sendBinary(bb);
            }
        } catch (Exception e) {
            session.getBasicRemote().sendText("fail: " + e);
            return;
        }

        session.getBasicRemote().sendText("done");
    }
}
