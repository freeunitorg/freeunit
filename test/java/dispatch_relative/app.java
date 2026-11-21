
import java.io.IOException;

import javax.servlet.RequestDispatcher;
import javax.servlet.ServletException;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

@WebServlet(urlPatterns = "")
public class app extends HttpServlet
{
    @Override
    public void doGet(HttpServletRequest request, HttpServletResponse response)
        throws IOException, ServletException
    {
        response.setHeader("X-Servlet-Path", "" + request.getServletPath());

        RequestDispatcher d = request.getRequestDispatcher("page.txt");

        if ("include".equals(request.getParameter("m"))) {
            d.include(request, response);

        } else {
            d.forward(request, response);
        }
    }
}
