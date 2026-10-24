import java.io.IOException;
import java.util.Collections;

import javax.websocket.HandshakeResponse;
import javax.websocket.OnMessage;
import javax.websocket.Session;
import javax.websocket.server.HandshakeRequest;
import javax.websocket.server.ServerEndpoint;
import javax.websocket.server.ServerEndpointConfig;

/*
 * A text mirror whose configurator adds the "X-Handshake: 1" response header
 * in modifyHandshake().  UpgradeUtil.doUpgrade() calls Request.upgrade()
 * before it adds this header to the response.
 *
 * Keep the echo method: a POJO endpoint without an annotated method fails
 * the handshake with a NullPointerException in PojoEndpointBase.doOnOpen().
 */
@ServerEndpoint(value = "/", configurator = app.HeaderConfigurator.class)
public class app {

    public static class HeaderConfigurator
            extends ServerEndpointConfig.Configurator {

        @Override
        public void modifyHandshake(ServerEndpointConfig sec,
                HandshakeRequest request, HandshakeResponse response) {
            response.getHeaders().put("X-Handshake",
                    Collections.singletonList("1"));
        }
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
}
