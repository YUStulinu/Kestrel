// A private HTTP cache on disk (RFC 9111, the parts a browser needs):
// freshness from Cache-Control max-age / Expires / a Last-Modified
// heuristic, no-store and no-cache, and revalidation with ETag and
// Last-Modified validators.
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type entry struct {
	URL      string              `json:"url"`
	Status   int                 `json:"status"`
	Header   map[string][]string `json:"header"`
	StoredAt time.Time           `json:"stored_at"`
}

// Cache wraps a client with a disk cache.
type Cache struct {
	Dir    string
	Client *Client
	Now    func() time.Time
	mu     sync.Mutex
	Log    []LogEntry
}

// LogEntry records how each request was answered (for the devtools).
type LogEntry struct {
	URL    string  `json:"url"`
	Status int     `json:"status"`
	Bytes  int     `json:"bytes"`
	Source string  `json:"source"` // network, cache, revalidated
	Millis float64 `json:"ms"`
}

func NewCache(dir string, c *Client) *Cache {
	os.MkdirAll(dir, 0o755)
	return &Cache{Dir: dir, Client: c, Now: time.Now}
}

func (c *Cache) paths(u string) (string, string) {
	h := sha256.Sum256([]byte(u))
	k := hex.EncodeToString(h[:16])
	return filepath.Join(c.Dir, k+".json"), filepath.Join(c.Dir, k+".body")
}

func directives(h map[string][]string) map[string]string {
	out := map[string]string{}
	for _, v := range h["cache-control"] {
		for _, d := range strings.Split(v, ",") {
			d = strings.TrimSpace(strings.ToLower(d))
			if d == "" {
				continue
			}
			if i := strings.IndexByte(d, '='); i >= 0 {
				out[d[:i]] = strings.Trim(d[i+1:], `"`)
			} else {
				out[d] = ""
			}
		}
	}
	return out
}

func first(h map[string][]string, k string) string {
	if v := h[k]; len(v) > 0 {
		return v[0]
	}
	return ""
}

// freshness lifetime in seconds (negative: must revalidate)
func lifetime(e *entry) float64 {
	d := directives(e.Header)
	if _, ok := d["no-cache"]; ok {
		return -1
	}
	if v, ok := d["max-age"]; ok {
		if n, err := strconv.Atoi(v); err == nil {
			return float64(n)
		}
	}
	if exp := first(e.Header, "expires"); exp != "" {
		t, err := http.ParseTime(exp)
		if err != nil {
			return -1
		}
		date := e.StoredAt
		if ds := first(e.Header, "date"); ds != "" {
			if dt, err := http.ParseTime(ds); err == nil {
				date = dt
			}
		}
		return t.Sub(date).Seconds()
	}
	if lm := first(e.Header, "last-modified"); lm != "" {
		if t, err := http.ParseTime(lm); err == nil {
			return e.StoredAt.Sub(t).Seconds() / 10
		}
	}
	return 0
}

func (c *Cache) load(u string) (*entry, []byte) {
	mp, bp := c.paths(u)
	m, err := os.ReadFile(mp)
	if err != nil {
		return nil, nil
	}
	var e entry
	if json.Unmarshal(m, &e) != nil {
		return nil, nil
	}
	b, err := os.ReadFile(bp)
	if err != nil {
		return nil, nil
	}
	return &e, b
}

func (c *Cache) store(u string, r *Response) {
	d := directives(r.Header)
	if _, ok := d["no-store"]; ok {
		return
	}
	if r.Status != 200 && r.Status != 203 && r.Status != 301 && r.Status != 404 {
		return
	}
	e := entry{URL: u, Status: r.Status, Header: r.Header, StoredAt: c.Now()}
	mp, bp := c.paths(u)
	m, _ := json.Marshal(e)
	os.WriteFile(bp, r.Body, 0o644)
	os.WriteFile(mp, m, 0o644)
}

func (c *Cache) logf(e LogEntry) {
	c.mu.Lock()
	c.Log = append(c.Log, e)
	c.mu.Unlock()
}

// Get answers from the cache when fresh, revalidates when stale, else fetches.
func (c *Cache) Get(u string) (*Response, error) {
	start := time.Now()
	e, body := c.load(u)
	if e != nil {
		age := c.Now().Sub(e.StoredAt).Seconds()
		if lt := lifetime(e); lt > 0 && age < lt {
			r := &Response{Status: e.Status, Header: e.Header, Body: body, URL: u, FromConn: "cache"}
			c.logf(LogEntry{u, r.Status, len(body), "cache", time.Since(start).Seconds() * 1000})
			return r, nil
		}
		// conditional request
		extra := map[string]string{}
		if et := first(e.Header, "etag"); et != "" {
			extra["If-None-Match"] = et
		}
		if lm := first(e.Header, "last-modified"); lm != "" {
			extra["If-Modified-Since"] = lm
		}
		if len(extra) > 0 {
			r, err := c.Client.Fetch(u, extra)
			if err == nil && r.Status == 304 {
				// refresh the stored headers
				for k, v := range r.Header {
					e.Header[k] = v
				}
				e.StoredAt = c.Now()
				mp, _ := c.paths(u)
				m, _ := json.Marshal(e)
				os.WriteFile(mp, m, 0o644)
				out := &Response{Status: e.Status, Header: e.Header, Body: body, URL: u, FromConn: "revalidated"}
				c.logf(LogEntry{u, out.Status, len(body), "revalidated", time.Since(start).Seconds() * 1000})
				return out, nil
			}
			if err == nil {
				c.store(u, r)
				c.logf(LogEntry{u, r.Status, len(r.Body), "network", time.Since(start).Seconds() * 1000})
				return r, nil
			}
		}
	}
	r, err := c.Client.Fetch(u, nil)
	if err != nil {
		return nil, err
	}
	c.store(u, r)
	c.logf(LogEntry{u, r.Status, len(r.Body), "network", time.Since(start).Seconds() * 1000})
	return r, nil
}
