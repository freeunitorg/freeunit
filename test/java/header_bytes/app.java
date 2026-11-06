
import java.io.IOException;
import java.io.PrintWriter;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

@WebServlet("/")
public class app extends HttpServlet
{
    private static String units(String s)
    {
        if (s == null) {
            return "null";
        }

        StringBuilder b = new StringBuilder();

        for (int i = 0; i < s.length(); i++) {
            if (i > 0) {
                b.append(',');
            }

            b.append(Integer.toHexString(s.charAt(i)));
        }

        return b.toString();
    }

    @Override
    public void doGet(HttpServletRequest request, HttpServletResponse response)
        throws IOException, ServletException
    {
        response.setContentType("text/plain");

        PrintWriter out = response.getWriter();

        out.println("header=" + units(request.getHeader("X-Header")));
        out.println("uri=" + units(request.getRequestURI()));
        out.println("query=" + units(request.getQueryString()));
    }
}
