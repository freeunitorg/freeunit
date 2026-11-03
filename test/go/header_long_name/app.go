package main

import (
	"io"
	"net/http"
	"strings"
	"unit.nginx.org/go"
)

func handler(w http.ResponseWriter, r *http.Request) {
	// A header name has a one byte length in the response.  This name
	// is 300 bytes long, so the module must not send it.
	w.Header().Set("X-"+strings.Repeat("a", 298), "1")
	w.Header().Set("X-Short", "1")

	io.WriteString(w, "body")
}

func main() {
	http.HandleFunc("/", handler)
	unit.ListenAndServe(":8080", nil)
}
