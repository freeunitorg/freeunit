import java.io.*;

import javax.servlet.ServletException;
import javax.servlet.ServletInputStream;
import javax.servlet.annotation.WebServlet;
import javax.servlet.http.HttpServlet;
import javax.servlet.http.HttpServletRequest;
import javax.servlet.http.HttpServletResponse;

/*
 * Calls ServletInputStream.readLine(buf, off, len) with a length that
 * overruns the target array. The native readLine (nxt_jni_InputStream.c)
 * must reject the out-of-bounds (off, len) rather than perform an OOB write
 * past the array's heap allocation. Post-fix that surfaces as an
 * IllegalStateException; the worker must stay alive.
 */
@WebServlet("/")
public class app extends HttpServlet
{
    @Override
    public void doPost(HttpServletRequest request, HttpServletResponse response)
        throws IOException, ServletException
    {
        ServletInputStream in = request.getInputStream();
        byte[] buf = new byte[8];
        String result;

        try {
            /* len (1000) far exceeds buf.length (8) -> must be rejected. */
            in.readLine(buf, 0, 1000);
            result = "no-exception";

        } catch (IllegalStateException e) {
            result = "rejected";

        } catch (Throwable t) {
            result = "other:" + t.getClass().getSimpleName();
        }

        byte[] out = result.getBytes("UTF-8");

        response.setContentType("text/plain");
        response.setContentLength(out.length);
        response.getOutputStream().write(out);
    }
}
