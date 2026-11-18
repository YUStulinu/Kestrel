// The devtools server: static files for the TypeScript panel and an API
// that renders a page with the engine and returns the inspection data.
package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type renderReq struct {
	URL    string `json:"url"`
	HTML   string `json:"html"`
	Page   string `json:"page"`
	Width  int    `json:"width"`
	Height int    `json:"height"`
}

var listenAddr = ":8091"

func serve(addr, kestrel, web, pages string, cache *Cache) {
	listenAddr = addr
	mux := http.NewServeMux()
	mux.HandleFunc("/api/pages", func(w http.ResponseWriter, r *http.Request) {
		names := []string{}
		ents, _ := os.ReadDir(pages)
		for _, e := range ents {
			if strings.HasSuffix(e.Name(), ".html") {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		writeJSON(w, names)
	})
	mux.HandleFunc("/api/render", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		var req renderReq
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		res, err := renderPage(kestrel, pages, req)
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		writeJSON(w, res)
	})
	mux.HandleFunc("/api/network", func(w http.ResponseWriter, r *http.Request) {
		cache.mu.Lock()
		defer cache.mu.Unlock()
		writeJSON(w, cache.Log)
	})
	// the example pages over HTTP, so the engine loads them through this network layer
	mux.Handle("/pages/", http.StripPrefix("/pages/", http.FileServer(http.Dir(pages))))
	mux.Handle("/", http.FileServer(http.Dir(web)))
	log.Printf("devtools on http://localhost%s", addr)
	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	log.Fatal(srv.ListenAndServe())
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

// renderPage runs `kestrel inspect` and returns its JSON with the PNG inlined.
func renderPage(kestrel, pages string, req renderReq) (map[string]any, error) {
	dir, err := os.MkdirTemp("", "kestrel-render")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	input := req.URL
	switch {
	case req.HTML != "":
		input = filepath.Join(dir, "page.html")
		if err := os.WriteFile(input, []byte(req.HTML), 0o644); err != nil {
			return nil, err
		}
	case req.Page != "":
		// fetched over HTTP from this server
		input = fmt.Sprintf("http://localhost%s/pages/%s", listenAddr, filepath.Base(req.Page))
	}
	if input == "" {
		return nil, fmt.Errorf("nothing to render")
	}
	if req.Width <= 0 {
		req.Width = 800
	}
	if req.Height <= 0 {
		req.Height = 600
	}
	png := filepath.Join(dir, "out.png")
	js := filepath.Join(dir, "out.json")
	cmd := exec.Command(kestrel, "inspect", input, "-o", png, "--json", js, "--width", fmt.Sprint(req.Width), "--height", fmt.Sprint(req.Height))
	cmd.Env = append(os.Environ(), "KESTREL_NET="+self())
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("kestrel failed: %v: %s", err, out)
	}
	var res map[string]any
	b, err := os.ReadFile(js)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(b, &res); err != nil {
		return nil, err
	}
	img, err := os.ReadFile(png)
	if err != nil {
		return nil, err
	}
	res["png"] = base64.StdEncoding.EncodeToString(img)
	return res, nil
}

func self() string {
	p, err := os.Executable()
	if err != nil {
		return "kestrel-net"
	}
	return p
}
