
import java.io.IOException;

import javax.servlet.ServletException;
import javax.servlet.annotation.WebFilter;
import javax.servlet.http.HttpServletResponse;
import javax.servlet.Filter;
import javax.servlet.FilterChain;
import javax.servlet.FilterConfig;
import javax.servlet.ServletRequest;
import javax.servlet.ServletResponse;

public class app
{
    @WebFilter(urlPatterns = "/admin/*")
    public static class admin_filter implements Filter
    {
        @Override
        public void init(FilterConfig filterConfig) { }

        @Override
        public void doFilter(ServletRequest request, ServletResponse response, FilterChain chain)
            throws IOException, ServletException
        {
            ((HttpServletResponse) response).sendError(403);
        }

        @Override
        public void destroy() { }
    }
}
