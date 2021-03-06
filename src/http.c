/*
 * A partial implementation of HTTP/1.0
 *
 * This code is mainly intended as a replacement for the book's 'tiny.c' server
 * It provides a *partial* implementation of HTTP/1.0 which can form a basis for
 * the assignment.
 * 
 * This project is a personal web server utilizing o the principles of internetwork communication using
 * the HTTP and TCP protocols and also implement a concurrent server that can handle multiple clients 
 * simultaneously. In session 2.1, we will implement the mime type to support some files like html, css, js
 * mp4. In the second session, support a single user that will authenticate with a username
 * and password. If the user is authenticated, they should have access to the secure portion
 * of your server, which are all files located under /private. Otherwise, such access should
 * be denied. In the session 2.3 and 2.4, we will implement the fall back html to change the URL 
 * that’s displayed in the address bar, making it appear to the user that they have navigated to a new URL 
 * when in fact all changes to the page were driven by JavaScript code that was originally loaded. We also support 
 * the range request to transfer part of a file
 *
 * @author G. Back for CS 3214 Spring 2018
 * @author Cuong Pham, Khanh Pham CS 3214 Spring 2022 (cpham006, khanh19)
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <dirent.h>

#include <jansson.h>
#include "http.h"
#include "hexdump.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"

// Need macros here because of the sizeof
#define CRLF "\r\n"
#define CR "\r"
#define STARTS_WITH(field_name, header) \
    (!strncasecmp(field_name, header, sizeof(header) - 1))
// static const char * NEVER_EMBED_A_SECRET_IN_CODE = "supa secret";
/* Parse HTTP request line, setting req_method, req_path, and req_version. */
static const char *NEVER_EMBED_A_SECRET_IN_CODE = "supa secret";
static bool
send_error(struct http_transaction *ta, enum http_response_status status, const char *fmt, ...);
static bool
http_parse_request(struct http_transaction *ta)
{
    size_t req_offset;
    ssize_t len = bufio_readline(ta->client->bufio, &req_offset);
    if (len < 2)
    { // error, EOF, or less than 2 characters
        return false;
    }

    char *request = bufio_offset2ptr(ta->client->bufio, req_offset);
    request[len - 2] = '\0'; // replace LF with 0 to ensure zero-termination
    char *endptr;
    char *method = strtok_r(request, " ", &endptr);
    if (method == NULL)
        return false;

    if (!strcmp(method, "GET"))
        ta->req_method = HTTP_GET;
    else if (!strcmp(method, "POST"))
        ta->req_method = HTTP_POST;
    else
        ta->req_method = HTTP_UNKNOWN;

    char *req_path = strtok_r(NULL, " ", &endptr);
    if (req_path == NULL)
    {
        return false;
    }
    else if (strstr(req_path, ".."))
    {
        return send_error(ta, HTTP_NOT_FOUND, "Permission denied.");
    }
    ta->req_path = bufio_ptr2offset(ta->client->bufio, req_path);

    char *http_version = strtok_r(NULL, CR, &endptr);
    if (http_version == NULL) // would be HTTP 0.9
        return false;

    // record client's HTTP version in request
    if (!strcmp(http_version, "HTTP/1.1"))
    {
        ta->req_version = HTTP_1_1;
    }
    else if (!strcmp(http_version, "HTTP/1.0"))
    {
        ta->req_version = HTTP_1_0;
    }
    else
    {
        return false;
    }

    return true;
}

/* Process HTTP headers. */
static bool
http_process_headers(struct http_transaction *ta)
{
    for (;;)
    {
        size_t header_offset;
        ssize_t len = bufio_readline(ta->client->bufio, &header_offset);
        if (len <= 0)
            return false;

        char *header = bufio_offset2ptr(ta->client->bufio, header_offset);
        if (len == 2 && STARTS_WITH(header, CRLF)) // empty CRLF
            return true;

        header[len - 2] = '\0';
        /* Each header field consists of a name followed by a
         * colon (":") and the field value. Field names are
         * case-insensitive. The field value MAY be preceded by
         * any amount of LWS, though a single SP is preferred.
         */
        char *endptr;
        char *field_name = strtok_r(header, ":", &endptr);
        if (field_name == NULL)
            return false;

        // skip white space
        char *field_value = endptr;
        while (*field_value == ' ' || *field_value == '\t')
            field_value++;

        // you may print the header like so
        // printf("Header: %s: %s\n", field_name, field_value);
        if (!strcasecmp(field_name, "Content-Length"))
        {
            ta->req_content_len = atoi(field_value);
        }
        if (!strcasecmp(field_name, "Range"))
        {
            ta->ispartial = true;
            char each[5];
            int s = -1;
            int e = -1;
            sscanf(field_value, "%5s=%d-%d", each, &s, &e);

            ta->start = s;
            ta->end = e;
            ta->unit = each;
        }
        if (!strcasecmp(field_name, "Cookie"))
        {
            char *body;
            char *method = strtok_r(field_value, ";", &body);
            while (method != NULL)
            {
                if (strstr(method, "auth_token"))
                {
                    break;
                }
                method = strtok_r(NULL, ";", &body);
            }
            strtok_r(method, "=", &body);
            ta->token = bufio_ptr2offset(ta->client->bufio, body);
            ta->check = true;
        }
        /* Handle other headers here. Both field_value and field_name
         * are zero-terminated strings.
         */
        if (!strcasecmp(field_name, "Connection"))
        {
            if (strcasecmp(field_value, "Keep-Alive") == 0)
                ta->checker = 0;
            else
                ta->checker = 1;
        }
    }
}

const int MAX_HEADER_LEN = 2048;

/* add a formatted header to the response buffer. */
void http_add_header(buffer_t *resp, char *key, char *fmt, ...)
{
    va_list ap;

    buffer_appends(resp, key);
    buffer_appends(resp, ": ");

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(resp, MAX_HEADER_LEN);
    int len = vsnprintf(error, MAX_HEADER_LEN, fmt, ap);
    resp->len += len > MAX_HEADER_LEN ? MAX_HEADER_LEN - 1 : len;
    va_end(ap);

    buffer_appends(resp, "\r\n");
}

/* add a content-length header. */
static void
add_content_length(buffer_t *res, size_t len)
{
    http_add_header(res, "Content-Length", "%ld", len);
}

/* start the response by writing the first line of the response
 * to the response buffer.  Used in send_response_header */
static void
start_response(struct http_transaction *ta, buffer_t *res)
{
    buffer_appends(res, "HTTP/1.1 ");

    switch (ta->resp_status)
    {
    case HTTP_OK:
        buffer_appends(res, "200 OK");
        break;
    case HTTP_PARTIAL_CONTENT:
        buffer_appends(res, "206 Partial Content");
        break;
    case HTTP_BAD_REQUEST:
        buffer_appends(res, "400 Bad Request");
        break;
    case HTTP_PERMISSION_DENIED:
        buffer_appends(res, "403 Permission Denied");
        break;
    case HTTP_NOT_FOUND:
        buffer_appends(res, "404 Not Found");
        break;
    case HTTP_METHOD_NOT_ALLOWED:
        buffer_appends(res, "405 Method Not Allowed");
        break;
    case HTTP_REQUEST_TIMEOUT:
        buffer_appends(res, "408 Request Timeout");
        break;
    case HTTP_REQUEST_TOO_LONG:
        buffer_appends(res, "414 Request Too Long");
        break;
    case HTTP_NOT_IMPLEMENTED:
        buffer_appends(res, "501 Not Implemented");
        break;
    case HTTP_SERVICE_UNAVAILABLE:
        buffer_appends(res, "503 Service Unavailable");
        break;
    case HTTP_INTERNAL_ERROR:
    default:
        buffer_appends(res, "500 Internal Server Error");
        break;
    }
    buffer_appends(res, CRLF);
}

/* Send response headers to client */
static bool
send_response_header(struct http_transaction *ta)
{
    buffer_t response;
    buffer_init(&response, 80);

    start_response(ta, &response);
    if (bufio_sendbuffer(ta->client->bufio, &response) == -1)
        return false;

    buffer_appends(&ta->resp_headers, CRLF);
    if (bufio_sendbuffer(ta->client->bufio, &ta->resp_headers) == -1)
        return false;

    buffer_delete(&response);
    return true;
}

/* Send a full response to client with the content in resp_body. */
static bool
send_response(struct http_transaction *ta)
{
    // add content-length.  All other headers must have already been set.
    add_content_length(&ta->resp_headers, ta->resp_body.len);

    if (!send_response_header(ta))
        return false;

    return bufio_sendbuffer(ta->client->bufio, &ta->resp_body) != -1;
}

const int MAX_ERROR_LEN = 2048;

/* Send an error response. */
static bool
send_error(struct http_transaction *ta, enum http_response_status status, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(&ta->resp_body, MAX_ERROR_LEN);
    int len = vsnprintf(error, MAX_ERROR_LEN, fmt, ap);
    ta->resp_body.len += len > MAX_ERROR_LEN ? MAX_ERROR_LEN - 1 : len;
    va_end(ap);
    ta->resp_status = status;
    http_add_header(&ta->resp_headers, "Content-Type", "text/plain");
    return send_response(ta);
}

/* Send Not Found response. */
static bool
send_not_found(struct http_transaction *ta)
{
    return send_error(ta, HTTP_NOT_FOUND, "File %s not found",
                      bufio_offset2ptr(ta->client->bufio, ta->req_path));
}

/* A start at assigning an appropriate mime type.  Real-world
 * servers use more extensive lists such as /etc/mime.types
 */
static const char *
guess_mime_type(char *filename)
{
    char *suffix = strrchr(filename, '.');
    if (suffix == NULL)
        return "text/plain";

    if (!strcasecmp(suffix, ".html"))
        return "text/html";

    if (!strcasecmp(suffix, ".gif"))
        return "image/gif";

    if (!strcasecmp(suffix, ".png"))
        return "image/png";

    if (!strcasecmp(suffix, ".jpg"))
        return "image/jpeg";

    if (!strcasecmp(suffix, ".js"))
        return "text/javascript";

    if (!strcasecmp(suffix, ".css"))
        return "text/css";

    if (!strcasecmp(suffix, ".mp4"))
        return "video/mp4";
    
    if (!strcasecmp(suffix, ".svg"))
        return "image/svg+xml";

    return "text/plain";
}

/* Handle HTTP transaction for static files. */
static bool
handle_static_asset(struct http_transaction *ta, char *basedir)
{
    char fname[PATH_MAX];
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);
    // The code below is vulnerable to an attack.  Can you see
    // which?  Fix it to avoid indirect object reference (IDOR) attacks.
    snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
    if (access(fname, R_OK))
    {
        if (errno == EACCES)
            return send_error(ta, HTTP_PERMISSION_DENIED, "Permission denied.");
        else if (html5_fallback)
        {
            char *encoded = bufio_offset2ptr(ta->client->bufio, ta->token);
            printf("%s\n", encoded);
            char *html_path = "/index.html";
            snprintf(fname, sizeof fname, "%s%s", basedir, html_path);
        }
        else
            return send_not_found(ta);
    }
    if (!strcmp(req_path, "/"))
    {
        if (html5_fallback)
        {
            char *html_path = "/index.html";
            snprintf(fname, sizeof fname, "%s%s", basedir, html_path);
        }
    }

    // Determine file size
    struct stat st;
    int rc = stat(fname, &st);
    if (rc == -1)
        return send_error(ta, HTTP_INTERNAL_ERROR, "Could not stat file.");

    int filefd = open(fname, O_RDONLY);
    if (filefd == -1)
    {
        return send_error(ta, HTTP_INTERNAL_ERROR, "Could not stat file.");
    }
    if (ta->ispartial != true)
    {
        ta->resp_status = HTTP_OK;
    }
    else
    {
        ta->resp_status = HTTP_PARTIAL_CONTENT;
    }

    http_add_header(&ta->resp_headers, "Content-Type", "%s", guess_mime_type(fname));
    off_t from, to, content_length;
    if (ta->ispartial)
    {
        from = ta->start;
        if (ta->end == -1)
        {
            to = st.st_size - 1;
            if (from < 0)
            {
                from = to + from + 1;
            }
        }
        else
        {
            to = ta->end;
        }

        content_length = to + 1 - from;
        http_add_header(&ta->resp_headers, "Content-Range", "bytes %ld-%ld/%ld", from, to, st.st_size);
    }
    else
    {
        from = 0;
        to = st.st_size - 1;
        content_length = to + 1 - from;
    }

    add_content_length(&ta->resp_headers, content_length);

    http_add_header(&ta->resp_headers, "Accept-Ranges", "bytes");
    ta->ispartial = false;
    bool success = send_response_header(ta);
    if (!success)
        goto out;

    // sendfile may send fewer bytes than requested, hence the loop
    while (success && from <= to)
        success = bufio_sendfile(ta->client->bufio, filefd, &from, to + 1 - from) > 0;

out:
    close(filefd);
    return success;
}

// This function will handle all the API, in the funcion we will check if the url is api/login and also check if the request method
// is post or get. If the password's value or username's value is NULL, we will send the error 403. Otherwise, we will send the response 
// body and also send the content of cookies. Also, we check if the URL is right but the request method is not post or get, we will send the 
// error 405. When the URL is api/video, it will take the all the mp4 file in root, and then return the json object which contains the 
// name and size
static bool
handle_api(struct http_transaction *ta, int expired)
{
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);
        // if url is api/login and req_method == POST
    if (strcmp(req_path, "/api/login") == 0 && ta->req_method == HTTP_POST)
    {
        char *body = bufio_offset2ptr(ta->client->bufio, ta->req_body);
        json_error_t json_err;
        json_t *obj = json_loadb(body, ta->req_body, JSON_DISABLE_EOF_CHECK, &json_err);
        json_t *username_json = json_object_get(obj, "username");
        if (username_json == NULL)
        {
            return send_error(ta, HTTP_PERMISSION_DENIED, "not right");
        }
        json_t *password_json = json_object_get(obj, "password");
        if (password_json == NULL)
        {
            return send_error(ta, HTTP_PERMISSION_DENIED, "not right");
        }
        const char *username = json_string_value(username_json);
        const char *password = json_string_value(password_json);

        if (strcmp(password, "thepassword") == 0 && strcmp(username, "user0") == 0)
        {
            // unsigned char public_key[16384];
            jwt_t *mytoken;
            jwt_new(&mytoken);

            jwt_add_grant(mytoken, "sub", username);
            time_t now = time(NULL);

            jwt_add_grant_int(mytoken, "iat", now);

            jwt_add_grant_int(mytoken, "exp", now + expired);

            jwt_set_alg(mytoken, JWT_ALG_HS256,
                        (unsigned char *)NEVER_EMBED_A_SECRET_IN_CODE,
                        strlen(NEVER_EMBED_A_SECRET_IN_CODE));

            char *encoded = jwt_encode_str(mytoken);

            char *grants = jwt_get_grants_json(mytoken, NULL);

            buffer_t response;
            buffer_init(&response, 80);
            ta->resp_status = HTTP_OK;
            buffer_appends(&ta->resp_body, grants);
            http_add_header(&ta->resp_headers, "Set-Cookie", "auth_token=%s; Path=/", encoded);
            http_add_header(&ta->resp_headers, "Content-Type", "%s", "application/json");
        }
        else
        {
            return send_error(ta, HTTP_PERMISSION_DENIED, "not right");
        }
    }
    // if url is api/login and req_method == GET
    else if (strcmp(req_path, "/api/login") == 0 && ta->req_method == HTTP_GET)
    {
        if (ta->check == true)
        {
            char *encoded = bufio_offset2ptr(ta->client->bufio, ta->token);
            jwt_t *ymtoken;
            time_t now = time(NULL);
            jwt_decode(&ymtoken, encoded,
                       (unsigned char *)NEVER_EMBED_A_SECRET_IN_CODE,
                       strlen(NEVER_EMBED_A_SECRET_IN_CODE));

            char *grants = jwt_get_grants_json(ymtoken, NULL);

            long expired = jwt_get_grant_int(ymtoken, "exp");
            ta->resp_status = HTTP_OK;
            if (now < expired)
            {
                buffer_appends(&ta->resp_body, grants);
            }
            else if (now > expired)
            {
                buffer_appends(&ta->resp_body, "{}");
            }
        }
        else
        {
            ta->resp_status = HTTP_OK;
            buffer_appends(&ta->resp_body, "{}");
        }
        http_add_header(&ta->resp_headers, "Content-Type", "%s", "application/json");
    }
    else if (strcmp(req_path, "/api/login") == 0 && ta->req_method != HTTP_GET && ta->req_method != HTTP_POST)
    {
        return send_error(ta, HTTP_METHOD_NOT_ALLOWED, "not right");
    }
    else if (strcmp(req_path, "/api/video") == 0 && ta->req_method == HTTP_GET)
    {
        DIR *dir;
        struct dirent *file;
        dir = opendir(server_root);
        char fname[PATH_MAX];
        json_t *video_array = json_array();
        if (video_array == NULL)
        {
            return send_error(ta, HTTP_INTERNAL_ERROR, "There is no object");
        }

        while ((file = readdir(dir)) != NULL)
        {
            char *suffix = file->d_name + strlen(file->d_name) - 4;
            if (suffix < file->d_name)
            {
                continue;
            }
            if (strcmp(suffix, ".mp4") == 0)
            {
                snprintf(fname, sizeof fname, "%s/%s", server_root, file->d_name);
                struct stat statis;
                stat(fname, &statis);
                json_t *video_object = json_object();
                int a = json_object_set_new(video_object, "size", json_integer(statis.st_size));
                int b = json_object_set_new(video_object, "name", json_string(file->d_name));
                if (a != 0)
                {
                    continue;
                }
                if (b != 0)
                {
                    continue;
                }
                json_array_append(video_array, video_object);
            }
        }
        closedir(dir);
        ta->resp_status = HTTP_OK;
        buffer_appends(&ta->resp_body, json_dumps(video_array, JSON_INDENT(4)));
        http_add_header(&ta->resp_headers, "Content-Type", "application/json");
        http_add_header(&ta->resp_headers, "Accept-Ranges", "bytes");
    }
    else
    {
        return send_error(ta, HTTP_NOT_FOUND, "not right");
    }

    return send_response(ta);
}

/* Set up an http client, associating it with a bufio buffer. */
void http_setup_client(struct http_client *self, struct bufio *bufio)
{
    self->bufio = bufio;
}

/* Handle a single HTTP transaction.  Returns true on success. */
bool http_handle_transaction(struct http_client *self, int expired)
{
    struct http_transaction ta;
    memset(&ta, 0, sizeof ta);
    ta.client = self;

    if (!http_parse_request(&ta))
        return false;

    if (!http_process_headers(&ta))
        return false;

    if (ta.req_content_len > 0)
    {
        int rc = bufio_read(self->bufio, ta.req_content_len, &ta.req_body);
        if (rc != ta.req_content_len)
            return false;

        // To see the body, use this:
        // char *body = bufio_offset2ptr(ta.client->bufio, ta.req_body);
        // printf("%s\n", body);
        // hexdump(body, ta.req_content_len);
    }

    buffer_init(&ta.resp_headers, 1024);
    http_add_header(&ta.resp_headers, "Server", "CS3214-Personal-Server");
    buffer_init(&ta.resp_body, 0);

    int rc = 0;
    char *req_path = bufio_offset2ptr(ta.client->bufio, ta.req_path);
    if (STARTS_WITH(req_path, "/api"))
    {
        rc = handle_api(&ta, expired);
    }
    else if (STARTS_WITH(req_path, "/private"))
    {
        if (ta.check == true)
        {
            char *encoded = bufio_offset2ptr(ta.client->bufio, ta.token);
            jwt_t *ymtoken;
            int dec = jwt_decode(&ymtoken, encoded,
                                   (unsigned char *)NEVER_EMBED_A_SECRET_IN_CODE,
                                   strlen(NEVER_EMBED_A_SECRET_IN_CODE));
            if (dec)
            {
                return send_error(&ta, HTTP_PERMISSION_DENIED, "not right");
            }
            long expired = jwt_get_grant_int(ymtoken, "exp");
            time_t now = time(NULL);
            if (expired < now)
            {
                return send_error(&ta, HTTP_PERMISSION_DENIED, "not right");
            }
            ta.resp_status = HTTP_OK;
            rc = handle_static_asset(&ta, server_root);
        }
        else
        {
            return send_error(&ta, HTTP_PERMISSION_DENIED, "not right");
        }
    }
    else
    {
        rc = handle_static_asset(&ta, server_root);
    }

    buffer_delete(&ta.resp_headers);
    buffer_delete(&ta.resp_body);
    if (ta.req_version == HTTP_1_0)
    {
        return false;
    }
    else if (ta.req_version == HTTP_1_1)
    {
        if (ta.checker == 1)
        {
            return false;
        }
        return true;
    }
    return rc;
}
