package main

import (
	"fmt"
	"net/http"
	"os"
	"strconv"

	"github.com/gin-gonic/gin"
)

func main() {
	host := "0.0.0.0"
	port := 9999

	if len(os.Args) > 1 {
		host = os.Args[1]
	}
	if len(os.Args) > 2 {
		if p, err := strconv.Atoi(os.Args[2]); err == nil {
			port = p
		}
	}

	gin.SetMode(gin.ReleaseMode)
	r := gin.New()

	r.GET("/health", func(c *gin.Context) {
		c.Data(http.StatusOK, "text/plain; charset=utf-8", []byte("ok"))
	})

	r.GET("/index", func(c *gin.Context) {
		c.Data(http.StatusOK, "text/plain; charset=utf-8", []byte("hello from gin server"))
	})

	r.NoRoute(func(c *gin.Context) {
		c.Data(http.StatusNotFound, "text/plain; charset=utf-8", []byte("not found"))
	})

	addr := fmt.Sprintf("%s:%d", host, port)
	fmt.Printf("gin benchmark server listening on http://%s\n", addr)
	fmt.Printf("endpoints: GET /health, GET /index\n")

	if err := r.Run(addr); err != nil {
		panic(err)
	}
}
