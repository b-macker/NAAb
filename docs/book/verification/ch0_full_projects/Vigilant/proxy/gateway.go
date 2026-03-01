// Vigilant/proxy/gateway.go
// PHASE 6: mTLS ZERO-TRUST GATEWAY

package main

import (
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"sync"
	"time"
)

const (
	SHIELD_SOCK  = "/data/data/com.termux/files/usr/tmp/v_s.sock"
	ANALYST_SOCK = "/data/data/com.termux/files/usr/tmp/v_a.sock"
	POLICY_FILE  = "/data/data/com.termux/files/home/.naab/language/docs/book/verification/ch0_full_projects/Vigilant/config/risk_matrix.json"
	
	// PKI Paths
	CA_CERT     = "/data/data/com.termux/files/home/.naab/language/docs/book/verification/ch0_full_projects/Vigilant/config/ca_cert.pem"
	SERVER_CERT = "/data/data/com.termux/files/home/.naab/language/docs/book/verification/ch0_full_projects/Vigilant/config/server_cert.pem"
	SERVER_KEY  = "/data/data/com.termux/files/home/.naab/language/docs/book/verification/ch0_full_projects/Vigilant/config/server_key.pem"
	
	// Legacy Auth (Secondary Layer)
	SOVEREIGN_KEY = "VIGILANT_SOVEREIGN_DEBUG_KEY_12345"
)

type Policy struct {
	Type   string `json:"type"`
	Score  int    `json:"score"`
}

type Config struct {
	Policies   []Policy `json:"policies"`
	Thresholds struct {
		Block  int `json:"block"`
		Redact int `json:"redact"`
	} `json:"thresholds"`
}

type Finding struct {
	Type string `json:"type"`
}

var globalConfig Config

func loadConfig() {
	data, err := os.ReadFile(POLICY_FILE)
	if err != nil { log.Fatalf("CONFIG_LOAD_FAIL: %v", err) }
	json.Unmarshal(data, &globalConfig)
}

func verifyIntegrity(path string) string {
	f, _ := os.Open(path)
	defer f.Close()
	h := sha256.New()
	io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil))
}

func scanWithDaemon(sockPath string, data []byte) ([]Finding, error) {
	conn, err := net.DialTimeout("unix", sockPath, 1*time.Second)
	if err != nil { return nil, err }
	defer conn.Close()

	conn.Write(data)
	if cw, ok := conn.(*net.UnixConn); ok { cw.CloseWrite() }

	resp, _ := io.ReadAll(conn)
	var findings []Finding
	json.Unmarshal(resp, &findings)
	return findings, nil
}

func handler(w http.ResponseWriter, r *http.Request) {
	// mTLS already verified the Identity.
	// We check the API Key as a second factor (Defense in Depth).
	clientKey := r.Header.Get("X-Vigilant-Auth")
	if clientKey != SOVEREIGN_KEY {
		w.WriteHeader(http.StatusUnauthorized)
		return
	}

	body, _ := io.ReadAll(r.Body)

	var wg sync.WaitGroup
	var rustFindings, pyFindings []Finding
	var rErr, pErr error

	wg.Add(2)
	go func() { defer wg.Done(); rustFindings, rErr = scanWithDaemon(SHIELD_SOCK, body) }()
	go func() { defer wg.Done(); pyFindings, pErr = scanWithDaemon(ANALYST_SOCK, body) }()
	wg.Wait()

	if rErr != nil || pErr != nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		return
	}

	all := append(rustFindings, pyFindings...)
	totalScore := 0
	for _, f := range all {
		for _, p := range globalConfig.Policies {
			if f.Type == p.Type { totalScore += p.Score }
		}
	}

	if totalScore >= globalConfig.Thresholds.Block {
		log.Printf("[SECURITY_BLOCK] Score: %d", totalScore)
		w.WriteHeader(http.StatusForbidden)
		w.Write([]byte("{\"error\": \"Enterprise Policy Violation\"}"))
		return
	}

	w.WriteHeader(http.StatusOK)
	w.Write([]byte("{\"status\": \"SECURE_PASS\"}"))
}

func main() {
	loadConfig()
	fmt.Printf("VIGILANT v3.1 [mTLS_ENABLED] Integrity: %s\n", verifyIntegrity(os.Args[0]))

	// mTLS Configuration
	caCert, err := os.ReadFile(CA_CERT)
	if err != nil { log.Fatal(err) }
	caCertPool := x509.NewCertPool()
	caCertPool.AppendCertsFromPEM(caCert)

	tlsConfig := &tls.Config{
		ClientCAs:  caCertPool,
		ClientAuth: tls.RequireAndVerifyClientCert, // THE IRON GATE
		MinVersion: tls.VersionTLS13,
	}

	server := &http.Server{
		Addr:      ":8091",
		Handler:   http.HandlerFunc(handler),
		TLSConfig: tlsConfig,
	}

	log.Fatal(server.ListenAndServeTLS(SERVER_CERT, SERVER_KEY))
}
