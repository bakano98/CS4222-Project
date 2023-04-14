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
#define SAMPLING_INTERVAL RTIMER_SECOND * 5 // remember to change to 30s sampling interval

#define MAX_NODES 5 // modify to specify max number of nodes that can be in proximity
#define NUM_DATA 10 // modify this to increase the number of experiments -- minimum is 10.

// For neighbour discovery, we would like to send message to everyone. We use Broadcast address:
linkaddr_t dest_addr;

/* WJ: link address = 0012.4b00.12b9.6687 = {{0x00, 0x12, 0x4b, 0x00, 0x12, 0xb9, 0x66, 0x87}} */
/* CH: link address = 0012.4b00.1665.f587 = {{0x00, 0x12, 0x4b, 0x00, 0x16, 0x65, 0xf5, 0x87}} */

static linkaddr_t light_addr =        {{0x00, 0x12, 0x4b, 0x00, 0x16, 0x65, 0xf5, 0x87}}; // modify this to change the light-sensing node

static int prev_sample_time = -1;

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
  unsigned long oop_timestamp;
  unsigned long last_sent;
} packet_store_struct;

typedef struct {
  int data[10];
} light_data_arr;

/*---------------------------------------------------------------------------*/
// duty cycle = WAKE_TIME / (WAKE_TIME + SLEEP_SLOT * SLEEP_CYCLE)
/*---------------------------------------------------------------------------*/

// sender timer implemented using rtimer
static struct rtimer rt;

// Protothread variable
static struct pt pt;
static struct pt light_pt;

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
// static packet_store_struct current;
// static unsigned int counter = 0;

// array to store sampling data
static int light_data[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int light_data_counter = 0;

// neighbours discovered so far. we allow discovery of up to 5 neighbours
packet_store_struct node_tracker[MAX_NODES];
static int node_counter = 0;


// Starts the main contiki neighbour discovery process
PROCESS(nbr_discovery_process, "cc2650 neighbour discovery process");
// PROCESS(process_light_sensor, "light sensor");
AUTOSTART_PROCESSES(&nbr_discovery_process);


/*========================start light sensor stuff========================*/

// static struct rtimer timer_rtimer;
// static rtimer_clock_t timeout_rtimer = SAMPLING_INTERVAL;

/*---------------------------------------------------------------------------*/
static void init_opt_reading(void);
static void get_light_reading(void);
char schedule_sleep(struct rtimer *t, void *ptr);
/*---------------------------------------------------------------------------*/
// void
// do_rtimer_timeout(struct rtimer *timer, void *ptr)
// {
//   /* Re-arm rtimer. Starting up the sensor takes around 125ms */
//   /* rtimer period 2s */
//   // clock_time_t t;

//   // light-sensing routine
//   rtimer_set(&timer_rtimer, RTIMER_NOW() + timeout_rtimer, 0, do_rtimer_timeout, NULL);


//   int s, ms1,ms2,ms3;
//   s = clock_time() / CLOCK_SECOND;
//   ms1 = (clock_time()% CLOCK_SECOND)*10/CLOCK_SECOND;
//   ms2 = ((clock_time()% CLOCK_SECOND)*100/CLOCK_SECOND)%10;
//   ms3 = ((clock_time()% CLOCK_SECOND)*1000/CLOCK_SECOND)%10;
  
//   printf(": %d (cnt) %ld (ticks) %d.%d%d%d (sec) \n",light_data_counter,clock_time(), s, ms1,ms2,ms3); 
//   get_light_reading();
// }

static void
get_light_reading()
{
  int value;
  value = opt_3001_sensor.value(0);
  if(value != CC26XX_SENSOR_READING_ERROR) {
    light_data[light_data_counter%10] = value;
    printf("OPT: Light=%d.%02d lux\n", value / 100, value % 100);
  } else {
    printf("OPT: Light Sensor's Warming Up\n\n");
  }
  init_opt_reading();
  light_data_counter++;
}

static void
init_opt_reading(void)
{
  SENSORS_ACTIVATE(opt_3001_sensor);
}

// Receiver gets data properly -- but needs syncing.
static void
send_light_data(const linkaddr_t *dest) {
  printf("Sending to address: ");
  LOG_INFO_LLADDR(dest);
  printf("\n");
  light_data_arr temp;
  for (int i = 0; i < 10; i++) {
    temp.data[i] = light_data[i];
  }
  nullnet_buf = (uint8_t *)&temp;
  nullnet_len = sizeof(temp);
  NETSTACK_NETWORK.output(dest);
  printf("Light: ");
  for (int i = 0; i < 10; i++) {
    printf("Reading %d = %d.%02d LUX ", i+1, light_data[i]/100, light_data[i]%100);
  }
  printf("\n");
}

/*========================end light sensor stuff========================*/


// Function called after reception of a packet
// Right now, it is always-on so it's not very good for power consumption -- how can we improve this?
void receive_packet_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) 
{
  // Check if the received packet size matches with what we expect it to be
  if(len == sizeof(data_packet)) {
    static data_packet_struct received_packet_data;
    int in_proximity = 0; // checks if this is in proximity
    int i;
    curr_timestamp = clock_time();

    // Copy the content of packet into the data structure
    memcpy(&received_packet_data, data, len);
    signed short recv_rssi = (signed short)packetbuf_attr(PACKETBUF_ATTR_RSSI);

    printf("Received neighbour discovery packet %lu with rssi %d from %ld\n", received_packet_data.seq, recv_rssi,received_packet_data.src_id);

    if (recv_rssi > -65) {
      in_proximity = 1;
    }


    packet_store_struct temp;
    temp.src_id = received_packet_data.src_id;
    temp.recv_timestamp = in_proximity ? curr_timestamp : 0;
    temp.oop_timestamp = in_proximity ? 0 : curr_timestamp;
    
    if (node_counter == 0) {
      printf("First node, no need to check\n");
      node_tracker[node_counter] = temp;
      node_counter++;
      return;
    }

    packet_store_struct *curr = NULL;

    // get a reference to the current packet's node
    for (i = 0; i < node_counter; i++) {
      if (node_tracker[i].src_id == temp.src_id) {
        curr = &node_tracker[i];
        break;
      }
    }

    if (curr == NULL) {
      // means this is a new node, so just add it in
      // and return
      node_tracker[node_counter] = temp;
      node_counter++;
      return;
    } 

    if (curr == NULL && node_counter == MAX_NODES) {
      printf("Max capacity reached\n");
    }

    // not a new node means we need to check the times
    unsigned long time_diff;
    // check either >= 15s if present, >= 30s if not
    if (in_proximity) {
      if (curr->recv_timestamp == 0) {
        printf("Recently got resetted\n");
        curr->recv_timestamp = curr_timestamp;
        curr->last_sent = 0;
      }
      
      if (curr->recv_timestamp == 0) {
        curr->recv_timestamp = curr_timestamp;
      }

      time_diff = curr_timestamp - curr->recv_timestamp;

      // change to 15
      if (time_diff / CLOCK_SECOND >= 2) {
        // send if it has been more than 30s since the last send
        if ((curr_timestamp - curr->last_sent) / CLOCK_SECOND >= 30 || curr->last_sent == 0) {
          printf("%3lu.%03lu DETECT %ld\n", curr->recv_timestamp / CLOCK_SECOND, ((curr->recv_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND, curr->src_id);
          send_light_data(src);
          curr->last_sent = curr_timestamp;
        }
      }
    } 
    // not in proximity
    else {
      if (curr->oop_timestamp == 0) {
        curr->oop_timestamp = curr_timestamp;
      }
      time_diff = curr_timestamp - curr->oop_timestamp;

      if (time_diff / CLOCK_SECOND >= 10) { // change to 30 for 30s
        // because it was otherwise never present.
        if (curr->recv_timestamp != 0 && curr->oop_timestamp != 0) {
          printf("%3lu.%03lu ABSENT %ld\n", curr->oop_timestamp / CLOCK_SECOND, ((curr->oop_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND, curr->src_id);
          curr->recv_timestamp = 0;
          curr->oop_timestamp = 0; // need to reset oop_timestamp
        } 
      }
      // maybe remove node if can, but for now do nothing
    }

  }

}

// // this is the sleeping function for 
char schedule_sleep(struct rtimer *t, void *ptr) {
  // printf("Schedule sleep for receiver\n");
  PT_BEGIN(&light_pt);
  static int i = 0;
  static int j;
  while (1) {

    NETSTACK_RADIO.on();

    // stay awake for 0.1s
    for(i = 0; i < NUM_SEND; i++){

      // no need to send
      // wait for WAKE_TIME before going into sleep routine
      if(i != (NUM_SEND - 1)){
        rtimer_set(t, RTIMER_TIME(t) + WAKE_TIME, 1, (rtimer_callback_t)schedule_sleep, ptr);
        PT_YIELD(&light_pt);
      }
   
    }

    // sleep for 9 slots
    NETSTACK_RADIO.off();
    for (j = 9; j > 0; j--) {
      // printf(" Sleep for 9 slots first before going into NEXT routine\n");
      rtimer_set(t, RTIMER_TIME(t) + SLEEP_SLOT, 1, (rtimer_callback_t)schedule_sleep, ptr);
      PT_YIELD(&light_pt);
    }
    // printf("Current time: %3lu.%03lu\n", clock_time() / CLOCK_SECOND, ((clock_time() % CLOCK_SECOND)*1000));

    if (prev_sample_time == -1) {
      printf("Collecting first light reading\n");
      get_light_reading();
      prev_sample_time = RTIMER_NOW();
    }

    if (RTIMER_NOW() - prev_sample_time  >= SAMPLING_INTERVAL) {
      printf("Interval reached, collecting reading\n");
      get_light_reading();
      prev_sample_time = RTIMER_NOW();
    }

  }

  PT_END(&light_pt);
}

//   printf("Sleep time over\n");
//   PT_END(&light_pt);
// }

// Scheduler function for the sender of neighbour discovery packets
char sender_scheduler(struct rtimer *t, void *ptr) {
 
  static uint16_t i = 0;
  static int sleep_counter = 0;
  static int sc;
  static int j;
  static int info;
  // Begin the protothread
  PT_BEGIN(&pt);

  // Get the current time stamp
  curr_timestamp = clock_time();

  printf("Start clock %lu ticks, timestamp %3lu.%03lu\n", curr_timestamp, curr_timestamp / CLOCK_SECOND, 
  ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);

  start_clock_time =  curr_timestamp;
  
  // total sleep for 0.9s, wake for 0.1s in a 1s period
  while(1){
    sc = sleep_counter % 9;
    NETSTACK_RADIO.off();
    for (j = 0; j < sc; j++) {
      printf(" Sleep for %d slots first before doing SEND routine\n", sc);
      rtimer_set(t, RTIMER_TIME(t) + SLEEP_SLOT, 1, (rtimer_callback_t)sender_scheduler, ptr);
      PT_YIELD(&pt);
    }

    // radio on
    NETSTACK_RADIO.on();

    // send NUM_SEND number of neighbour discovery beacon packets
    for(i = 0; i < NUM_SEND; i++){

     
      // Initialize the nullnet module with information of packet to be trasnmitted
      nullnet_buf = (uint8_t *)&data_packet; //data transmitted
      nullnet_len = sizeof(data_packet); //length of data transmitted

      data_packet.seq++;
      
      curr_timestamp = clock_time();
      
      data_packet.timestamp = curr_timestamp;

      // printf("Send seq# %lu  @ %8lu ticks   %3lu.%03lu\n", data_packet.seq, curr_timestamp, curr_timestamp / CLOCK_SECOND, ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);

      NETSTACK_NETWORK.output(&light_addr); //Packet transmission
      

      // wait for WAKE_TIME before sending the next packet
      if(i != (NUM_SEND - 1)){

        rtimer_set(t, RTIMER_TIME(t) + WAKE_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
        PT_YIELD(&pt);
      
      }
   
    }

    NETSTACK_RADIO.off();
    info = 9 - sc;
    for (j = 9 - sc; j > 0; j--) {
      printf(" Sleep for %d slots first before going into NEXT routine\n", info);
      rtimer_set(t, RTIMER_TIME(t) + SLEEP_SLOT, 1, (rtimer_callback_t)sender_scheduler, ptr);
      PT_YIELD(&pt);
    }
    sleep_counter++;
    printf("Current time: %3lu.%03lu\n", clock_time() / CLOCK_SECOND, ((clock_time() % CLOCK_SECOND)*1000));
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
  if (linkaddr_cmp(&light_addr, &linkaddr_node_addr)) {
    printf("============ This is the light sensing node ============\n\n");
    init_opt_reading();
    rtimer_set(&rt, RTIMER_NOW() + RTIMER_SECOND, 1,  (rtimer_callback_t)schedule_sleep, NULL);
  } else {
    // Start sender in one millisecond.
    rtimer_set(&rt, RTIMER_NOW() + (RTIMER_SECOND / 1000), 1, (rtimer_callback_t)sender_scheduler, NULL);
  }

  PROCESS_END();
}

