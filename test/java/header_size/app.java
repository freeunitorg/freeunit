
import java.io.IOException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

@WebServlet("/")
public class app extends HttpServlet
{
    private static String repeat(char c, int n)
    {
        StringBuilder sb = new StringBuilder(n);

        for (int i = 0; i < n; i++) {
            sb.append(c);
        }

        return sb.toString();
    }

    @Override
    public void doGet(HttpServletRequest request, HttpServletResponse response)
        throws IOException, ServletException
    {
        if (request.getRequestURI().equals("/large")) {
            /*
             * The module first asks for 10 KB for the headers.  This
             * value needs more than one doubling of the buffer.
             */
            response.addHeader("X-Large", repeat('x', 100000));

        } else {
            /*
             * A header name has a one byte length in the response.  These
             * names are 300 bytes long, so the module must not send them.
             */
            response.setHeader("X-" + repeat('a', 298), "1");
            response.addHeader("X-" + repeat('b', 298), "1");
            response.addIntHeader("X-" + repeat('c', 298), 1);
        }

        response.setHeader("X-Short", "1");
    }
}
