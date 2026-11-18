// An HTTP/1.1 client written on raw connections: request serialization,
// response parsing (status line, headers, Content-Length, chunked transfer
// coding, gzip/deflate bodies), keep-alive pooling and redirects.
package main

import (
	"bufio"
	"bytes"
	"compress/flate"
	"compress/gzip"
	"crypto/tls"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Response is a fully read HTTP response.
type Response struct {
	Status   int
	Reason   string
	Header   map[string][]string // canonical lower-case names
	Body     []byte
	URL      string // the final URL after redirects
	FromConn string // "new", "reused" or "cache"
}

// Get returns the first value of a header.
func (r *Response) Get(name string) string {
	v := r.Header[strings.ToLower(name)]
	if len(v) == 0 {
		return ""
	}
	return v[0]
}

type pooled struct {
	conn net.Conn
	br   *bufio.Reader
}

// Client keeps idle connections per host:port.
type Client struct {
	mu           sync.Mutex
	idle         map[string][]*pooled
	Timeout      time.Duration
	MaxRedirects int
	UserAgent    string
	TLSConfig    *tls.Config
}

func NewClient() *Client {
	return &Client{idle: map[string][]*pooled{}, Timeout: 20 * time.Second, MaxRedirects: 10, UserAgent: "Kestrel/0.1"}
}

func hostKey(u *url.URL) string {
	port := u.Port()
	if port == "" {
		if u.Scheme == "https" {
			port = "443"
		} else {
			port = "80"
		}
	}
	return u.Scheme + "://" + u.Hostname() + ":" + port
}

func (c *Client) dial(u *url.URL) (*pooled, bool, error) {
	key := hostKey(u)
	c.mu.Lock()
	if l := c.idle[key]; len(l) > 0 {
		p := l[len(l)-1]
		c.idle[key] = l[:len(l)-1]
		c.mu.Unlock()
		return p, true, nil
	}
	c.mu.Unlock()
	addr := key[strings.Index(key, "://")+3:]
	d := net.Dialer{Timeout: c.Timeout}
	var conn net.Conn
	var err error
	if u.Scheme == "https" {
		cfg := c.TLSConfig
		if cfg == nil {
			cfg = &tls.Config{}
		}
		cfg = cfg.Clone()
		cfg.ServerName = u.Hostname()
		conn, err = tls.DialWithDialer(&d, "tcp", addr, cfg)
	} else {
		conn, err = d.Dial("tcp", addr)
	}
	if err != nil {
		return nil, false, err
	}
	return &pooled{conn: conn, br: bufio.NewReader(conn)}, false, nil
}

func (c *Client) release(u *url.URL, p *pooled) {
	key := hostKey(u)
	c.mu.Lock()
	c.idle[key] = append(c.idle[key], p)
	c.mu.Unlock()
}

// Do sends one request (no redirects). extra headers are added as given.
func (c *Client) Do(method, rawurl string, extra map[string]string, body []byte) (*Response, error) {
	u, err := url.Parse(rawurl)
	if err != nil {
		return nil, err
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return nil, fmt.Errorf("unsupported scheme %q", u.Scheme)
	}
	// a reused connection may have been closed by the server: retry once on a new one
	for attempt := 0; attempt < 2; attempt++ {
		p, reused, err := c.dial(u)
		if err != nil {
			return nil, err
		}
		resp, keep, err := c.roundTrip(p, u, method, extra, body)
		if err != nil {
			p.conn.Close()
			if reused && attempt == 0 {
				continue
			}
			return nil, err
		}
		if reused {
			resp.FromConn = "reused"
		} else {
			resp.FromConn = "new"
		}
		if keep {
			c.release(u, p)
		} else {
			p.conn.Close()
		}
		resp.URL = u.String()
		return resp, nil
	}
	return nil, errors.New("unreachable")
}

func (c *Client) roundTrip(p *pooled, u *url.URL, method string, extra map[string]string, body []byte) (*Response, bool, error) {
	p.conn.SetDeadline(time.Now().Add(c.Timeout))
	path := u.EscapedPath()
	if path == "" {
		path = "/"
	}
	if u.RawQuery != "" {
		path += "?" + u.RawQuery
	}
	var b bytes.Buffer
	fmt.Fprintf(&b, "%s %s HTTP/1.1\r\n", method, path)
	host := u.Host
	fmt.Fprintf(&b, "Host: %s\r\n", host)
	fmt.Fprintf(&b, "User-Agent: %s\r\n", c.UserAgent)
	b.WriteString("Accept: */*\r\nAccept-Encoding: gzip, deflate\r\nConnection: keep-alive\r\n")
	for k, v := range extra {
		fmt.Fprintf(&b, "%s: %s\r\n", k, v)
	}
	if body != nil {
		fmt.Fprintf(&b, "Content-Length: %d\r\n", len(body))
	}
	b.WriteString("\r\n")
	b.Write(body)
	if _, err := p.conn.Write(b.Bytes()); err != nil {
		return nil, false, err
	}
	return readResponse(p.br, method)
}

// readResponse parses a response from the reader; it returns whether the
// connection may be reused.
func readResponse(br *bufio.Reader, method string) (*Response, bool, error) {
	line, err := readLine(br)
	if err != nil {
		return nil, false, err
	}
	// 1xx informational responses are skipped
	for strings.HasPrefix(line, "HTTP/1.1 1") || strings.HasPrefix(line, "HTTP/1.0 1") {
		for {
			l, err := readLine(br)
			if err != nil {
				return nil, false, err
			}
			if l == "" {
				break
			}
		}
		if line, err = readLine(br); err != nil {
			return nil, false, err
		}
	}
	parts := strings.SplitN(line, " ", 3)
	if len(parts) < 2 || !strings.HasPrefix(parts[0], "HTTP/") {
		return nil, false, fmt.Errorf("malformed status line %q", line)
	}
	code, err := strconv.Atoi(parts[1])
	if err != nil {
		return nil, false, fmt.Errorf("malformed status code %q", parts[1])
	}
	resp := &Response{Status: code, Header: map[string][]string{}}
	if len(parts) == 3 {
		resp.Reason = parts[2]
	}
	http10 := parts[0] == "HTTP/1.0"
	for {
		l, err := readLine(br)
		if err != nil {
			return nil, false, err
		}
		if l == "" {
			break
		}
		i := strings.IndexByte(l, ':')
		if i <= 0 {
			continue
		}
		name := strings.ToLower(strings.TrimSpace(l[:i]))
		resp.Header[name] = append(resp.Header[name], strings.TrimSpace(l[i+1:]))
	}
	keep := !http10
	if strings.EqualFold(resp.Get("connection"), "close") {
		keep = false
	}
	if strings.EqualFold(resp.Get("connection"), "keep-alive") {
		keep = true
	}
	// the body
	var raw []byte
	noBody := method == "HEAD" || code == 204 || code == 304 || (code >= 100 && code < 200)
	switch {
	case noBody:
	case strings.Contains(strings.ToLower(resp.Get("transfer-encoding")), "chunked"):
		raw, err = readChunked(br)
		if err != nil {
			return nil, false, err
		}
	case resp.Get("content-length") != "":
		n, err := strconv.ParseInt(resp.Get("content-length"), 10, 64)
		if err != nil || n < 0 {
			return nil, false, fmt.Errorf("bad Content-Length")
		}
		raw = make([]byte, n)
		if _, err := io.ReadFull(br, raw); err != nil {
			return nil, false, err
		}
	default:
		// read until the connection closes
		raw, err = io.ReadAll(br)
		if err != nil {
			return nil, false, err
		}
		keep = false
	}
	switch strings.ToLower(resp.Get("content-encoding")) {
	case "gzip", "x-gzip":
		zr, err := gzip.NewReader(bytes.NewReader(raw))
		if err != nil {
			return nil, false, err
		}
		if raw, err = io.ReadAll(zr); err != nil {
			return nil, false, err
		}
	case "deflate":
		// zlib-wrapped (RFC 1950) per the spec; some servers send raw deflate
		data := raw
		if len(data) >= 2 && (uint16(data[0])<<8|uint16(data[1]))%31 == 0 {
			data = data[2:]
		}
		if raw, err = io.ReadAll(flate.NewReader(bytes.NewReader(data))); err != nil {
			return nil, false, err
		}
	}
	resp.Body = raw
	return resp, keep, nil
}

func readLine(br *bufio.Reader) (string, error) {
	l, err := br.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(l, "\r\n"), nil
}

func readChunked(br *bufio.Reader) ([]byte, error) {
	var out bytes.Buffer
	for {
		l, err := readLine(br)
		if err != nil {
			return nil, err
		}
		if i := strings.IndexByte(l, ';'); i >= 0 {
			l = l[:i]
		}
		n, err := strconv.ParseInt(strings.TrimSpace(l), 16, 64)
		if err != nil || n < 0 {
			return nil, fmt.Errorf("bad chunk size %q", l)
		}
		if n == 0 {
			// trailers until an empty line
			for {
				t, err := readLine(br)
				if err != nil {
					return nil, err
				}
				if t == "" {
					return out.Bytes(), nil
				}
			}
		}
		if _, err := io.CopyN(&out, br, n); err != nil {
			return nil, err
		}
		if _, err := readLine(br); err != nil {
			return nil, err
		}
	}
}

// Fetch follows redirects (301, 302, 303, 307, 308).
func (c *Client) Fetch(rawurl string, extra map[string]string) (*Response, error) {
	cur := rawurl
	for i := 0; i <= c.MaxRedirects; i++ {
		resp, err := c.Do("GET", cur, extra, nil)
		if err != nil {
			return nil, err
		}
		switch resp.Status {
		case 301, 302, 303, 307, 308:
			loc := resp.Get("location")
			if loc == "" {
				return resp, nil
			}
			base, _ := url.Parse(cur)
			next, err := base.Parse(loc)
			if err != nil {
				return nil, err
			}
			cur = next.String()
			continue
		}
		return resp, nil
	}
	return nil, fmt.Errorf("too many redirects")
}
