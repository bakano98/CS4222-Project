/*
* CS4222/5422: Assignment 3b
* Perform neighbour discovery
*/

#include "contiki.h"
#include "net/netstack.h"
#include "net/nullnet/nullnet.h"
#include "net/packetbuf.h"
#include "lib/random.h"
#include "net/linkaddr.h"
#include <string.h>
#include <stdio.h> 
#include "node-id.h"
#include "board-peripherals.h"
#include <stdint.h>
#include "sys/rtimer.h"


#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO
// Identification information of the node


// Configures the wake-up timer for neighbour discovery 
#define WAKE_TIME RTIMER_SECOND/10    // 10 HZ, 0.1s
#define SLEEP_CYCLE  9        	      // 0 for never sleep
#define SLEEP_SLOT RTIMER_SECOND/10   // sleep slot should not be too large to prevent overflow
#define SAMPLING_INTERVAL RTIMER_SECOND * 30 // 30s sampling interval


#define NUM_DATA 10 // modify this to increase the number of experiments -- minimum is 10.

// For neighbour discovery, we would like to send message to everyone. We use Broadcast address:
linkaddr_t dest_addr;

// WJ: link address = 0012.4b00.12b9.6687 = {{0x00, 0x12, 0x4b, 0x00, 0x12, 0xb9, 0x66, 0x87}}
// CH: link address = 0012.4b00.1665.f587 = {{0x00, 0x12, 0x4b, 0x00, 0x16, 0x65, 0xf5, 0x87}}
static linkaddr_t light_addr = { {0x00, 0x12, 0x4b, 0x00, 0x12, 0xb9, 0x66, 0x87} }; // we use WJ's node as the one with light sensor


#define NUM_SEND 2
/*---------------------------------------------------------------------------*/
typedef struct {
  unsigned long src_id;
  unsigned long timestamp;
  unsigned long seq;

} data_packet_struct;


/*---------------------------------------------------------------------------*/
// custom struct to store
typedef struct {
  unsigned long src_id;
  unsigned long recv_timestamp;
  unsigned long prev_timestamp;
} packet_store_struct;

/*---------------------------------------------------------------------------*/
// duty cycle = WAKE_TIME / (WAKE_TIME + SLEEP_SLOT * SLEEP_CYCLE)
/*---------------------------------------------------------------------------*/

// sender timer implemented using rtimer
static struct rtimer rt;

// Protothread variable
static struct pt pt;

// Structure holding the data to be transmitted
static data_packet_struct data_packet;

// Current time stamp of the node
unsigned long curr_timestamp;

// save timestamp when it started sending
unsigned long start_clock_time;

// save previous time it discovered the neighbour
unsigned long prev_discovery_timestamp = -1;

// array to store all the packets received so far -- experiment: 50 received packets then we stop
packet_store_struct storage[NUM_DATA];
static packet_store_struct current;
static unsigned int counter = 0;

// array to store sampling data
static int light_data[10] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int light_data_counter = 0;

// Starts the main contiki neighbour discovery process
PROCESS(nbr_discovery_process, "cc2650 neighbour discovery process");
PROCESS(process_light_sensor, "light sensor");
AUTOSTART_PROCESSES(&nbr_discovery_process);


/*========================start light sensor stuff========================*/

static struct rtimer timer_rtimer;
static rtimer_clock_t timeout_rtimer = SAMPLING_INTERVAL;

/*---------------------------------------------------------------------------*/
static void init_opt_reading(void);
static void get_light_reading(void);

/*---------------------------------------------------------------------------*/
void
do_rtimer_timeout(struct rtimer* timer, void* ptr)
{
  /* Re-arm rtimer. Starting up the sensor takes around 125ms */
  /* rtimer period 2s */
  // clock_time_t t;

  rtimer_set(&timer_rtimer, RTIMER_NOW() + timeout_rtimer, 0, do_rtimer_timeout, NULL);

  int s, ms1, ms2, ms3;
  s = clock_time() / CLOCK_SECOND;
  ms1 = (clock_time() % CLOCK_SECOND) * 10 / CLOCK_SECOND;
  ms2 = ((clock_time() % CLOCK_SECOND) * 100 / CLOCK_SECOND) % 10;
  ms3 = ((clock_time() % CLOCK_SECOND) * 1000 / CLOCK_SECOND) % 10;

  printf(": %d (cnt) %ld (ticks) %d.%d%d%d (sec) \n", light_data_counter, clock_time(), s, ms1, ms2, ms3);
  get_light_reading();
  light_data_counter++;
  if (light_data_counter != 0 && light_data_counter % 10 == 0) {
    printf("Hit 10 new data sets read, printing out reading...\n");
    for (int i = 0; i < 10; i++) {
      printf("Reading %d = %d.%02d LUX\n", i, light_data[i] / 100, light_data[i] % 100);
    }
  }
}

static void
get_light_reading()
{
  int value;
  value = opt_3001_sensor.value(0);
  if (value != CC26XX_SENSOR_READING_ERROR) {
    light_data[light_data_counter % 10] = value;
    printf("OPT: Light=%d.%02d lux\n", value / 100, value % 100);
  }
  else {
    printf("OPT: Light Sensor's Warming Up\n\n");
  }
  init_opt_reading();
}

static void
init_opt_reading(void)
{
  SENSORS_ACTIVATE(opt_3001_sensor);
}

/*========================end light sensor stuff========================*/


// Function called after reception of a packet
void receive_packet_callback(const void* data, uint16_t len, const linkaddr_t* src, const linkaddr_t* dest)
{

  // const linkaddr_t *src gives us the source address of the received packet. we can use this
  // to track how long we've been in contact with, and whether to send the data to them or not!

  // Check if the received packet size matches with what we expect it to be

  if (len == sizeof(data_packet)) {

    static data_packet_struct received_packet_data;

    curr_timestamp = clock_time();

    // gets the timestamp after offsetting the time it took to send
    unsigned long after_offset = curr_timestamp - start_clock_time;
    unsigned long diff = 0;
    // Copy the content of packet into the data structure
    memcpy(&received_packet_data, data, len);


    // Print the details of the received packet
    printf("========\n");
    printf("Received neighbour discovery packet %lu with rssi %d from %ld\n", received_packet_data.seq, (signed short)packetbuf_attr(PACKETBUF_ATTR_RSSI), received_packet_data.src_id);
    printf("========\n");

    printf("Current timestamp: %3lu.%03lu\n", curr_timestamp / CLOCK_SECOND, ((curr_timestamp % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
    printf("========\n");

    printf("Timestamp after accounting for offset: %3lu.%03lu\n", after_offset / CLOCK_SECOND, ((after_offset % CLOCK_SECOND) * 1000) / CLOCK_SECOND);

    if (prev_discovery_timestamp != -1) {
      diff = curr_timestamp - prev_discovery_timestamp;
      printf("Time difference between current and last discovery: %3lu.%03lu\n", diff / CLOCK_SECOND, ((diff % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
      printf("========\n");
    }
    else {
      printf("This is the first discovery -- no previous one yet!\n");
      printf("========\n");
    }

    if (counter < NUM_DATA) {
      current.src_id = received_packet_data.seq;
      current.recv_timestamp = after_offset;
      current.prev_timestamp = diff; // if diff = 0, means no previous
      storage[counter] = current;
    }

    prev_discovery_timestamp = curr_timestamp;
    counter++;

    if (counter == NUM_DATA) {
      printf("| Number | Packet Number | Received Time | Last Received Time |\n");
      for (int i = 0; i < NUM_DATA; i++) {
        unsigned long id = storage[i].src_id;
        unsigned long recv_time = storage[i].recv_timestamp;
        unsigned long prev_time = storage[i].prev_timestamp;
        printf("|   %d   |      %lu       |   %3lu.%03lu  |      %3lu.%03lu    |\n", i, id,
          recv_time / CLOCK_SECOND, ((recv_time % CLOCK_SECOND) * 1000) / CLOCK_SECOND,
          prev_time / CLOCK_SECOND, ((prev_time % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
      }
    }
  }

}

// Scheduler function for the sender of neighbour discovery packets
char sender_scheduler(struct rtimer* t, void* ptr) {

  static uint16_t i = 0;

  static int NumSleep = 0;

  // Begin the protothread
  PT_BEGIN(&pt);

  // Get the current time stamp
  curr_timestamp = clock_time();

  printf("Start clock %lu ticks, timestamp %3lu.%03lu\n", curr_timestamp, curr_timestamp / CLOCK_SECOND,
    ((curr_timestamp % CLOCK_SECOND) * 1000) / CLOCK_SECOND);

  start_clock_time = curr_timestamp;

  while (1) {

    // radio on
    NETSTACK_RADIO.on();

    // send NUM_SEND number of neighbour discovery beacon packets
    for (i = 0; i < NUM_SEND; i++) {


      // Initialize the nullnet module with information of packet to be trasnmitted
      nullnet_buf = (uint8_t*)&data_packet; //data transmitted
      nullnet_len = sizeof(data_packet); //length of data transmitted

      data_packet.seq++;

      curr_timestamp = clock_time();

      data_packet.timestamp = curr_timestamp;

      // printf("Send seq# %lu  @ %8lu ticks   %3lu.%03lu\n", data_packet.seq, curr_timestamp, curr_timestamp / CLOCK_SECOND, ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);

      NETSTACK_NETWORK.output(&dest_addr); //Packet transmission


      // wait for WAKE_TIME before sending the next packet
      if (i != (NUM_SEND - 1)) {

        rtimer_set(t, RTIMER_TIME(t) + WAKE_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
        PT_YIELD(&pt);

      }

    }

    // sleep for a random number of slots
    if (SLEEP_CYCLE != 0) {

      // radio off
      NETSTACK_RADIO.off();

      // SLEEP_SLOT cannot be too large as value will overflow,
      // to have a large sleep interval, sleep many times instead

      // get a value that is uniformly distributed between 0 and 2*SLEEP_CYCLE
      // the average is SLEEP_CYCLE 
      NumSleep = random_rand() % (2 * SLEEP_CYCLE + 1);

      // uncomment if necessary -- probably not! 
      // printf(" Sleep for %d slots \n",NumSleep); 

      // NumSleep should be a constant or static int
      for (i = 0; i < NumSleep; i++) {
        rtimer_set(t, RTIMER_TIME(t) + SLEEP_SLOT, 1, (rtimer_callback_t)sender_scheduler, ptr);
        PT_YIELD(&pt);
      }

    }
  }

  PT_END(&pt);
}


// Main thread that handles the neighbour discovery process
PROCESS_THREAD(nbr_discovery_process, ev, data)
{

  // static struct etimer periodic_timer;

  PROCESS_BEGIN();

  // initialize data packet sent for neighbour discovery exchange
  data_packet.src_id = node_id; //Initialize the node ID
  data_packet.seq = 0; //Initialize the sequence number of the packet

  nullnet_set_input_callback(receive_packet_callback); //initialize receiver callback
  linkaddr_copy(&dest_addr, &linkaddr_null);



  printf("CC2650 neighbour discovery\n");
  printf("Node %d will be sending packet of size %d Bytes\n", node_id, (int)sizeof(data_packet_struct));


  // if it is the light sensor node, keep collecting data very SAMPLING_INTERVAL
  // next: send current data on neighbour discovery?
  if (linkaddr_cmp(&light_addr, &linkaddr_node_addr)) {
    printf("============ This is the light sensing node\n\n ============");
    process_start(&process_light_sensor, NULL);
  }
  else {
    // Start sender in one millisecond.
    rtimer_set(&rt, RTIMER_NOW() + (RTIMER_SECOND / 1000), 1, (rtimer_callback_t)sender_scheduler, NULL);
  }

  PROCESS_END();
}


PROCESS_THREAD(process_light_sensor, ev, data)
{
  PROCESS_BEGIN();
  init_opt_reading();

  printf(" The value of RTIMER_SECOND is %d \n", RTIMER_SECOND);
  printf(" The value of timeout_rtimer is %ld \n", timeout_rtimer);

  while (1) {
    rtimer_set(&timer_rtimer, RTIMER_NOW() + timeout_rtimer, 0, do_rtimer_timeout, NULL);
    PROCESS_YIELD();
  }

  PROCESS_END();
}
