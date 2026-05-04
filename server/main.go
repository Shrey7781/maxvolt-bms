package main

import (
	"context"
	"log"
	"net/http"
	"os"

	"github.com/gin-gonic/gin"
	"github.com/jackc/pgx/v5/pgxpool"
)

type BMSPayload struct {
	DeviceID  string    `json:"device_id"  binding:"required"`
	Voltage   float32   `json:"voltage"`
	Current   float32   `json:"current"`
	Power     float32   `json:"power"`
	SOC       int16     `json:"soc"`
	SOH       int16     `json:"soh"`
	RemainMAh int32     `json:"remain_mah"`
	FullMAh   int32     `json:"full_mah"`
	Cycles    int32     `json:"cycles"`
	TempMOS   float32   `json:"temp_mos"`
	TempBat   []float32 `json:"temp_bat"`
	Cells     []int32   `json:"cells"`
	Charging  bool      `json:"charging"`
}

var db *pgxpool.Pool

func postBMS(c *gin.Context) {
	var p BMSPayload
	if err := c.ShouldBindJSON(&p); err != nil {
		c.Status(http.StatusBadRequest)
		return
	}

	_, err := db.Exec(c.Request.Context(), `
		INSERT INTO bms_readings
			(device_id, voltage, current, power, soc, soh,
			 remain_mah, full_mah, cycles, temp_mos, temp_bat, cells, charging)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)`,
		p.DeviceID, p.Voltage, p.Current, p.Power, p.SOC, p.SOH,
		p.RemainMAh, p.FullMAh, p.Cycles, p.TempMOS, p.TempBat, p.Cells, p.Charging,
	)
	if err != nil {
		log.Printf("db insert: %v", err)
		c.Status(http.StatusInternalServerError)
		return
	}

	c.Status(http.StatusNoContent)
}

func main() {
	dsn := os.Getenv("DATABASE_URL")
	if dsn == "" {
		dsn = "postgres://postgres:password@localhost:5432/maxvolt?sslmode=disable"
	}

	var err error
	db, err = pgxpool.New(context.Background(), dsn)
	if err != nil {
		log.Fatalf("db connect: %v", err)
	}
	defer db.Close()

	if err = db.Ping(context.Background()); err != nil {
		log.Fatalf("db ping: %v", err)
	}
	log.Println("database connected")

	gin.SetMode(gin.ReleaseMode)
	r := gin.New()
	r.Use(gin.Logger(), gin.Recovery())

	r.POST("/bms", postBMS)

	log.Println("gateway listening on :8080")
	log.Fatal(r.Run(":8080"))
}
