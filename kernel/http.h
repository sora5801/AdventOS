#ifndef ADVENTOS_HTTP_H
#define ADVENTOS_HTTP_H

/*
 * Tiny HTTP/1.0 server: listens on port 80, on first inbound segment
 * sends a canned 200 OK response, then closes. No request parsing
 * beyond logging the start of the request line for the demo.
 */
void http_init(void);

#endif
