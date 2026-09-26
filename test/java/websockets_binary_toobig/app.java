import java.io.IOException;
import java.nio.ByteBuffer;

import javax.websocket.OnMessage;
import javax.websocket.OnOpen;
import javax.websocket.PongMessage;
import javax.websocket.Session;
import javax.websocket.server.ServerEndpoint;

/*
 * A minimal mirror app with a small binary buffer, so a fragmented binary
 * message that overflows it can be exercised with a handful of bytes
 * instead of the 16 MiB the shared websockets_mirror app now requires
 * (see issue #435).
 */
@ServerEndpoint("/")
public class app {

    @OnOpen
    public void onOpen(Session session) {
        session.setMaxBinaryMessageBufferSize(1024);
    }

    @OnMessage
    public void echoTextMessage(Session session, String msg) {
        try {
            if (session.isOpen()) {
                session.getBasicRemote().sendText(msg, true);
            }
        } catch (IOException e) {
            try {
                session.close();
            } catch (IOException e1) {
                // Ignore
            }
        }
    }

    @OnMessage
    public void echoBinaryMessage(Session session, ByteBuffer bb) {
        try {
            if (session.isOpen()) {
                session.getBasicRemote().sendBinary(bb, true);
            }
        } catch (IOException e) {
            try {
                session.close();
            } catch (IOException e1) {
                // Ignore
            }
        }
    }

    @OnMessage
    public void echoPongMessage(PongMessage pm) {
        // NO-OP
    }
}
