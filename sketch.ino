#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ---------------- Pin Definitions ---------------- */
#define PIN_ESTOP        2U    /* External Interrupt INT0 (Active LOW button) */
#define PIN_SAFETY_SW    3U    /* Physical Safety Switch (Active LOW = ON)    */
#define PIN_RELAY        4U    /* Relay Module IN (Active HIGH)               */

#define KEY_ROWS         4U
#define KEY_COLS         4U

/* ---------------- System Constants ---------------- */
static const char     CORRECT_PIN[]  = "1234";
static const uint32_t RELAY_PULSE_MS = 3000UL;  /* Relay ON duration: 3 s */
static const uint32_t TICK_MS        = 1000UL;  /* 1-second timer tick    */

static const uint8_t ROW_PINS[KEY_ROWS] = {12, 11, 10, 9};
static const uint8_t COL_PINS[KEY_COLS] = {8, 7, 6, 5};

static const char KEYMAP[KEY_ROWS][KEY_COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

/* ---------------- 4-State Deterministic FSM ---------------- */
typedef enum {
  STATE_IDLE = 0,
  STATE_ARMED,
  STATE_COUNTDOWN,
  STATE_ABORT
} system_state_t;

static system_state_t g_state = STATE_IDLE;

/* ---------------- Shared ISR Flag ---------------- */
/* volatile forces fresh RAM reads so loop() always sees ISR updates */
static volatile bool g_estop_flag = false;

/* ---------------- LCD Driver Instance ---------------- */
static LiquidCrystal_I2C lcd(0x27, 16, 2);

/* ---------------- Fixed-Size C Runtime Buffers ---------------- */
static char     g_pin_buf[5]       = "";
static uint8_t  g_pin_len          = 0;
static char     g_delay_buf[3]     = "";
static uint8_t  g_delay_len        = 0;
static char     g_abort_reason[17] = "";

static int      g_remaining_sec    = 0;
static uint32_t g_last_tick_ms     = 0;
static uint32_t g_relay_start_ms   = 0;
static bool     g_relay_firing     = false;
static bool     g_last_sw_state    = false;

/* ---------------- Hardware Helper Functions ---------------- */
static bool is_safety_switch_on(void) {
  return (digitalRead(PIN_SAFETY_SW) == LOW);
}

/* External Interrupt ISR (INT0 on Pin 2): minimal, fast, no Serial/I2C calls */
static void isr_emergency_stop(void) {
  digitalWrite(PIN_RELAY, LOW);   /* Instant hardware cut-off */
  g_estop_flag = true;
}

/* Pure-C non-blocking 4x4 Matrix Keypad Scanner (no Keypad library needed) */
static char keypad_scan(void) {
  static char     last_key      = '\0';
  static uint32_t last_press_ms = 0;
  char detected = '\0';

  for (uint8_t r = 0; r < KEY_ROWS; r++) {
    digitalWrite(ROW_PINS[r], LOW);
    delayMicroseconds(5);
    for (uint8_t c = 0; c < KEY_COLS; c++) {
      if (digitalRead(COL_PINS[c]) == LOW) {
        detected = KEYMAP[r][c];
        break;
      }
    }
    digitalWrite(ROW_PINS[r], HIGH);
    if (detected != '\0') {
      break;
    }
  }

  if (detected != '\0') {
    uint32_t now = millis();
    if ((detected != last_key) && ((now - last_press_ms) >= 50UL)) {
      last_key      = detected;
      last_press_ms = now;
      return detected;
    }
  } else {
    last_key = '\0';
  }

  return '\0';
}

/* ---------------- LCD Display Functions ---------------- */
static void render_idle_screen(const char *notice) {
  char line[17];
  lcd.clear();
  lcd.setCursor(0, 0);

  if (notice != NULL) {
    lcd.print(notice);
  } else {
    snprintf(line, sizeof(line), "IDLE  SW:%s", is_safety_switch_on() ? "ON " : "OFF");
    lcd.print(line);
  }

  lcd.setCursor(0, 1);
  lcd.print("PIN: ");
  for (uint8_t i = 0; i < g_pin_len; i++) {
    lcd.print('*');
  }
}

static void render_armed_screen(void) {
  char line[17];
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ARMED (SW: ON)");
  lcd.setCursor(0, 1);
  snprintf(line, sizeof(line), "SEC(1-99): %s", g_delay_buf);
  lcd.print(line);
}

static void render_countdown_screen(void) {
  char line[17];
  lcd.clear();
  lcd.setCursor(0, 0);

  if (g_relay_firing) {
    lcd.print("RELAY TRIGGERED!");
    lcd.setCursor(0, 1);
    lcd.print("OUTPUT ENERGIZED");
  } else {
    lcd.print("STATE: COUNTDOWN");
    lcd.setCursor(0, 1);
    snprintf(line, sizeof(line), "T-MINUS: %2d s", g_remaining_sec);
    lcd.print(line);
  }
}

static void render_abort_screen(void) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("!! ABORTED !!");
  lcd.setCursor(0, 1);
  lcd.print(g_abort_reason);
}

static void trigger_abort(const char *reason) {
  digitalWrite(PIN_RELAY, LOW);
  g_relay_firing = false;
  g_state        = STATE_ABORT;

  g_pin_len      = 0;
  g_pin_buf[0]   = '\0';
  g_delay_len    = 0;
  g_delay_buf[0] = '\0';

  strncpy(g_abort_reason, reason, sizeof(g_abort_reason) - 1U);
  g_abort_reason[sizeof(g_abort_reason) - 1U] = '\0';

  Serial.print("[FSM] -> STATE_ABORT | Reason: ");
  Serial.println(g_abort_reason);
  render_abort_screen();
}

/* ---------------- Initialization ---------------- */
void setup(void) {
  /* Force relay pin LOW first to avoid power-on glitches */
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);

  pinMode(PIN_ESTOP, INPUT_PULLUP);
  pinMode(PIN_SAFETY_SW, INPUT_PULLUP);

  /* Initialize 4x4 keypad rows (OUTPUT HIGH) and columns (INPUT_PULLUP) */
  for (uint8_t r = 0; r < KEY_ROWS; r++) {
    pinMode(ROW_PINS[r], OUTPUT);
    digitalWrite(ROW_PINS[r], HIGH);
  }
  for (uint8_t c = 0; c < KEY_COLS; c++) {
    pinMode(COL_PINS[c], INPUT_PULLUP);
  }

  Serial.begin(115200);
  lcd.init();
  lcd.backlight();

  /* Attach External Interrupt INT0 on Pin 2 */
  attachInterrupt(digitalPinToInterrupt(PIN_ESTOP), isr_emergency_stop, FALLING);

  g_last_sw_state = is_safety_switch_on();
  Serial.println("=== Safety Timer & Relay Controller Booted (STATE: IDLE) ===");
  render_idle_screen(NULL);
}

/* ---------------- Non-Blocking Superloop ---------------- */
void loop(void) {
  /* 1. Atomically consume E-Stop flag set inside ISR */
  bool estop_hit = false;
  noInterrupts();
  if (g_estop_flag) {
    g_estop_flag = false;
    estop_hit    = true;
  }
  interrupts();

  if (estop_hit && (g_state != STATE_ABORT)) {
    trigger_abort("E-STOP! #=RST");
    return;
  }

  /* 2. Continuous physical safety switch supervision */
  bool sw_on = is_safety_switch_on();
  if (((g_state == STATE_ARMED) || (g_state == STATE_COUNTDOWN)) && !sw_on) {
    trigger_abort("SW OFF! #=RST");
    return;
  }

  if ((g_state == STATE_IDLE) && (sw_on != g_last_sw_state)) {
    g_last_sw_state = sw_on;
    render_idle_screen(NULL);
  }

  /* 3. Non-blocking matrix keypad scan */
  char key = keypad_scan();

  /* 4. Deterministic 4-State FSM */
  switch (g_state) {

    case STATE_IDLE:
      if ((key >= '0') && (key <= '9') && (g_pin_len < 4U)) {
        g_pin_buf[g_pin_len++] = key;
        g_pin_buf[g_pin_len]   = '\0';
        render_idle_screen(NULL);

        if (g_pin_len == 4U) {
          bool pin_ok = (strcmp(g_pin_buf, CORRECT_PIN) == 0);
          g_pin_len    = 0;
          g_pin_buf[0] = '\0';

          if (pin_ok) {
            if (is_safety_switch_on()) {
              g_state        = STATE_ARMED;
              g_delay_len    = 0;
              g_delay_buf[0] = '\0';
              Serial.println("[FSM] PIN Valid + Safety Switch ON -> STATE_ARMED");
              render_armed_screen();
            } else {
              Serial.println("[SAFE] Valid PIN rejected: Safety Switch is OFF");
              render_idle_screen("TURN SAFETY ON!");
            }
          } else {
            Serial.println("[SAFE] Invalid PIN entered");
            render_idle_screen("WRONG PIN!");
          }
        }
      } else if (key == '*') {
        g_pin_len    = 0;
        g_pin_buf[0] = '\0';
        render_idle_screen(NULL);
      }
      break;

    case STATE_ARMED:
      if ((key >= '0') && (key <= '9') && (g_delay_len < 2U)) {
        g_delay_buf[g_delay_len++] = key;
        g_delay_buf[g_delay_len]   = '\0';
        render_armed_screen();
      } else if (key == '*') {
        g_delay_len    = 0;
        g_delay_buf[0] = '\0';
        render_armed_screen();
      } else if ((key == '#') && (g_delay_len > 0U)) {
        int sec = atoi(g_delay_buf);
        if ((sec >= 1) && (sec <= 99)) {
          g_remaining_sec = sec;
          g_relay_firing  = false;
          g_last_tick_ms  = millis();
          g_state         = STATE_COUNTDOWN;

          Serial.print("[FSM] -> STATE_COUNTDOWN | Delay: ");
          Serial.print(g_remaining_sec);
          Serial.println(" s");
          render_countdown_screen();
        }
      }
      break;

    case STATE_COUNTDOWN:
      if (key == '*') {
        trigger_abort("USER CANCEL");
        break;
      }

      if (!g_relay_firing) {
        if ((millis() - g_last_tick_ms) >= TICK_MS) {
          g_last_tick_ms += TICK_MS;
          g_remaining_sec--;

          if (g_remaining_sec <= 0) {
            if (is_safety_switch_on() && !g_estop_flag) {
              digitalWrite(PIN_RELAY, HIGH);
              g_relay_firing   = true;
              g_relay_start_ms = millis();
              Serial.println("[ACTUATOR] Countdown 0 -> RELAY ENERGIZED");
              render_countdown_screen();
            } else {
              trigger_abort("INTERLOCK ERR");
            }
          } else {
            Serial.print("[TIMER] T-minus ");
            Serial.print(g_remaining_sec);
            Serial.println(" s");
            render_countdown_screen();
          }
        }
      } else {
        if ((millis() - g_relay_start_ms) >= RELAY_PULSE_MS) {
          digitalWrite(PIN_RELAY, LOW);
          g_relay_firing  = false;
          g_state         = STATE_IDLE;
          g_last_sw_state = is_safety_switch_on();
          Serial.println("[ACTUATOR] Pulse complete -> RELAY OFF -> STATE_IDLE");
          render_idle_screen("CYCLE COMPLETE!");
        }
      }
      break;

    case STATE_ABORT:
      digitalWrite(PIN_RELAY, LOW);
      if (key == '#') {
        if (!is_safety_switch_on() && (digitalRead(PIN_ESTOP) == HIGH)) {
          g_state         = STATE_IDLE;
          g_last_sw_state = false;
          Serial.println("[FSM] Lockout Cleared -> STATE_IDLE");
          render_idle_screen(NULL);
        } else {
          lcd.setCursor(0, 1);
          lcd.print("TURN SW OFF!    ");
        }
      }
      break;
  }
}
