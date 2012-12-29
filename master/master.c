#define F_CPU 8000000UL 
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/twi.h>
#include <util/delay.h>
#include <util/crc16.h>

#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <stdarg.h>
#include <stdlib.h>

#include "../dispenser/defs.h"
#include "../dispenser/serial.h"
#include "../dispenser/packet.h"

#if F_CPU == 16000000UL
#define    TIMER1_INIT      0xFFEF
#define    TIMER1_FLAGS     _BV(CS12)|(1<<CS10); // 16Mhz / 1024 / 16 = .001024 per tick
#else
#define    TIMER1_INIT      0xFFF7
#define    TIMER1_FLAGS     _BV(CS12)|(1<<CS10); // 8Mhz / 1024 / 8 = .001024 per tick
#endif

#define MAX_DISPENSERS   15 
#define RESET_DURATION   1

void    set_pin(uint8_t port, uint8_t pin);
void    clear_pin(uint8_t port, uint8_t pin);
uint8_t get_port(uint8_t port);
uint8_t get_port_ddr(uint8_t port);
uint8_t get_pcmsk(uint8_t msk);

// TODO:
// THink about serial IO errors on this level.
// check for COLLISIONS in name assignment
// Ensure that I2C has error correction
// Implement dispenser select
// implement number of dispensers available
// Implement data driven interrupt routing

/*

   Test mappings:

   RPI:

       PD0 -> RESET input
       PB0 -> RX (pcint0)
       PB1 -> TX
       A4  -> SDA red wire
       A5  -> SCL green wire

   Dispenser 0:

       PD2 -> RESET
       PD1 -> TX
       PD3 -> RX (pcint19)
       PD7 -> SYNC

   Dispenser 1:

       PD2 -> RESET
       PD1 -> TX
       PD4 -> RX (pcint20)
       PD7 -> SYNC

*/

typedef struct 
{
    uint8_t reset_port, reset_pin;
    uint8_t rx_port, rx_pin, rx_pcint;
    uint8_t tx_port, tx_pin;
} dispenser_t;

static volatile dispenser_t dispensers[2] =
{  // reset_port, reset_pin, rx_port, rx_pin, rx_pcint, tx_port, tx_pin
    { 'D',        6,         'D',     5,      PCINT13,  'D',     3      },
    { 'D',        6,         'B',     0,      PCINT0,   'D',     5      },
};

volatile uint32_t g_time = 0;
static volatile uint32_t g_reset_fe_time = 0;
static volatile uint8_t  g_dispenser = 0;
static volatile uint8_t  g_mux_pin_0 = 0;
static volatile uint8_t  g_reset = 0;
static volatile uint8_t  g_dispenser_count = 0;

ISR(TWI_vect)
{
   uint8_t twi_status, data;

   // Get TWI Status Register, mask the prescaler bits (TWPS1,TWPS0)
   twi_status=TWSR & 0xF8;     
   switch(twi_status) 
   {
       case TW_SR_DATA_ACK:     // 0x80: data received, ACK returned
           data = TWDR;
           if (data == 255)
               g_reset = 1;
           if (data < g_dispenser_count)
               g_dispenser = data;

           break;
   }
   TWCR |= (1<<TWINT);    // Clear TWINT Flag
}

volatile uint8_t pcint0 = 0;
volatile uint8_t pcint1 = 0;
volatile uint8_t pcint2 = 0;

ISR(PCINT0_vect)
{
    uint8_t      state;

    // Check for RX from the RPI
    state = PINB & (1<<PINB0);
    if (state != pcint0)
    {
        if (state)
            sbi(PORTD, 1);
        else
            cbi(PORTD, 1);
        pcint0 = state;
    }
}

volatile uint8_t  pcint19 = 0;
volatile uint8_t  pcint20 = 0;
volatile uint32_t g_rx_pcint19_fe_time = 0;
volatile uint32_t g_rx_pcint20_fe_time = 0;
volatile uint8_t  g_dispenser_rx[MAX_DISPENSERS];
volatile uint8_t  g_in_id_assignment;
volatile uint8_t  g_sync = 0;

ISR(PCINT2_vect)
{
    uint8_t      state;

    // Check for RX for Dispenser 0
    state = PIND & (1<<PIND3);
    if (state != pcint19)
    {
        if (g_in_id_assignment)
        {
            if (state)
                g_rx_pcint19_fe_time = g_time + RESET_DURATION;
            else
            {
                if (g_rx_pcint19_fe_time > 0 && g_time >= g_rx_pcint19_fe_time)
                    g_dispenser_rx[0] = 1;
                g_rx_pcint19_fe_time = 0;
            }
        }
        else
        if (g_dispenser == 0)
        {
            if (state)
                sbi(PORTB, 1);
            else
                cbi(PORTB, 1);
        }
        pcint19 = state;
    }

    // Check for RX for Dispenser 1
    state = PIND & (1<<PIND4);
    if (state != pcint20)
    {
        if (g_in_id_assignment)
        {
            if (state)
                g_rx_pcint20_fe_time = g_time + RESET_DURATION;
            else
            {
                if (g_rx_pcint20_fe_time > 0 && g_time >= g_rx_pcint20_fe_time)
                    g_dispenser_rx[1] = 1;
                g_rx_pcint20_fe_time = 0;
            }
        }
        else
        if (g_dispenser == 1)
        {
            if (state)
                sbi(PORTB, 1);
            else
                cbi(PORTB, 1);
        }
        pcint20 = state;
    }
}

ISR (TIMER1_OVF_vect)
{
    g_time++;
    if (g_sync)
        tbi(PORTD, 7);
    TCNT1 = TIMER1_INIT;
}

uint8_t check_reset(void)
{
    return 0;
}

void reset_dispensers(void)
{
    // Reset the dispensers
    sbi(PORTD, 2);
    _delay_ms(RESET_DURATION + RESET_DURATION);
    cbi(PORTD, 2);

    // Wait for dispensers to start up
    _delay_ms(500);
    _delay_ms(500);
}

void set_pin(uint8_t port, uint8_t pin)
{
    switch(port)
    {
        case 'B':
            sbi(PORTB, pin);
            return;
        case 'C':
            sbi(PORTC, pin);
            return;
        case 'D': 
            sbi(PORTD, pin);
            return;
    }
}

void clear_pin(uint8_t port, uint8_t pin)
{
    switch(port)
    {
        case 'B':
            cbi(PORTB, pin);
            return;
        case 'C':
            cbi(PORTC, pin);
            return;
        case 'D': 
            cbi(PORTD, pin);
            return;
    }
}

uint8_t get_port(uint8_t port)
{
    switch(port)
    {
        case 1:
            return PORTB;
        case 2:
            return PORTC;
        case 3: 
            return PORTD;
        default:
            return 0xFF;
    }
}

uint8_t get_port_ddr(uint8_t port)
{
    switch(port)
    {
        case 1:
            return DDRB;
        case 2:
            return DDRC;
        case 3: 
            return DDRD;
        default:
            return 0xFF;
    }
}

void setup_ports(void)
{
    uint8_t i, ddr, port;

    for(i = 0; i < MAX_DISPENSERS; i++)
    {
        port = get_port(dispensers[i].reset_port);
        ddr = get_port_ddr(dispensers[i].reset_port);
        dispensers[i].reset_port = port;
        ddr |= (1 << dispensers[i].reset_pin);

        dispensers[i].rx_port = get_port(dispensers[i].rx_port);
//        PCMSK |= (1 << dispensers[1].rx_pcint);

        port = get_port(dispensers[i].tx_port);
        ddr = get_port_ddr(dispensers[i].tx_port);
        dispensers[i].tx_port = port;
        ddr |= (1 << dispensers[i].tx_pin);
    }
}

void setup(void)
{
//    setup_ports();

    // on board LED
    DDRB |= (1<< PORTB5);

    // TX to RPI
    DDRB |= (1<< PORTB1);
    // TX to dispensers
    DDRD |= (1<< PORTD2);

    // SYNC to dispensers
    DDRD |= (1<< PORTD7);

    // PCINT setup
    PCMSK0 |= (1 << PCINT0);
    PCMSK2 |= (1 << PCINT19) | (1 << PCINT20) ;
    PCICR |=  (1 << PCIE0) | (1 << PCIE2);

    // Timer setup for reset pulse width measuring
    TCCR1B |= TIMER1_FLAGS;
    TCNT1 = TIMER1_INIT;
    TIMSK1 |= (1<<TOIE1);

    // I2C setup
    TWAR = (1 << 3); // address
    TWDR = 0x0;  
    TWCR = (1<<TWEN) | (1<<TWIE) | (1<<TWEA);  

    sei();
}

void flash_led(uint8_t fast)
{
    int i;

    for(i = 0; i < 5; i++)
    {
        sbi(PORTB, 5);
        if (fast)
            _delay_ms(50);
        else
            _delay_ms(250);
        cbi(PORTB, 5);
        if (fast)
            _delay_ms(50);
        else
            _delay_ms(250);
    }
}

// TODO: handle collisions and missing dispensers
uint8_t setup_ids(void)
{
    uint8_t  i, j, state, count = 0;
    uint8_t  dispensers[MAX_DISPENSERS];

    serial_init();
    serial_enable(0, 1);

    cli();
    g_in_id_assignment = 1;
    sei();

    memset(dispensers, 0xFF, sizeof(dispensers));
    for(;;)
    {
        count = 0;
        for(i = 0; i < 255; i++)
        {
            cli();
            memset((void *)g_dispenser_rx, 0, sizeof(g_dispenser_rx));
            sei();

            serial_tx(i);
            _delay_ms(3);
            for(j = 0; j < MAX_DISPENSERS; j++)
            {
                cli();
                state = g_dispenser_rx[j];
                sei();
                if (state)
                {
                    dispensers[j] = i;
                    count++;
                }
            }
        }
#if 0
        flash_led(count == 2); //MAX_DISPENSERS);
        for(i = 0; i < min(count, 10); i++)
        {
            sbi(PORTB, 2);
            _delay_ms(200);
            cbi(PORTB, 2);
            _delay_ms(200);
        }
#endif
        if (count >= 0)
            break;

        // Found no dispensers!
        flash_led(0);
        flash_led(0);
        reset_dispensers();
    }
    _delay_ms(5);
    serial_tx(255);
    _delay_ms(5);

    for(i = 0; i < MAX_DISPENSERS; i++)
    {
        if (dispensers[i] != 255)
        {
            serial_tx(dispensers[i]);
            serial_tx(i);
        }
    }

    _delay_ms(5);
    serial_tx(255);
    _delay_ms(5);

    // Disable serial IO and put D2 back to output
    serial_enable(0, 0);
    DDRD |= (1<< PORTD2) | (1<< PORTD1);

    // start by pulling D1 & B1 high, since serial lines when idle are high
    sbi(PORTD, 1);
    sbi(PORTB, 1);

    cli();
    g_in_id_assignment = 0;
    sei();

    return count;
}

void idle(void)
{
}

int main (void)
{
    uint8_t reset = 0, count;

    for(;;)
    {
        DDRB |= (1 << PORTB5) | (1 << PORTB2);
        DDRD |= (1 << PORTD2);
        flash_led(1);
        g_sync = 0;

        reset_dispensers();
        setup();
        count = setup_ids();
        cli();
        g_dispenser_count = count;
        g_sync = 1;
        sei();
        for(;;)
        {
            cli();
            reset = g_reset;
            sei();

            if (reset)
            {
                cli();
                g_reset = 0;
                sei();
                break; 
            }
            _delay_ms(1);
        }
    }
    return 0;
}

