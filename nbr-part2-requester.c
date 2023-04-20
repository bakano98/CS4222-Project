/*
* CS4222/5422: Group Project
* Requesting Node - This is the node that sends discovery packets to the light sensing node.
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
#define TRUE 1
#define FALSE 0
#define DETECT 1
#define ABSENT 0

// Configures the wake-up timer for neighbour discovery 
#define SLOT_TIME RTIMER_SECOND/10    // 10 HZ, 0.1s
#define SLEEP_CYCLE  9        	      // 0 for never sleep
#define TOTAL_SLOTS SLEEP_CYCLE + 1
#define NUM_SEND 2

#define RSSI_WINDOW 5 // the number of rssi_values we want to keep
#define IN_PROXIMITY_THRESHOLD 15
#define OUT_OF_PROXIMITY_THRESHOLD 30
#define REQ 12345678

// For neighbour discovery, we would like to send message to everyone. We use Broadcast address:
linkaddr_t dest_addr;

/* WJ: link address = 0012.4b00.12b9.6687 = {{0x00, 0x12, 0x4b, 0x00, 0x12, 0xb9, 0x66, 0x87}} */
/* CH: link address = 0012.4b00.1665.f587 = {{0x00, 0x12, 0x4b, 0x00, 0x16, 0x65, 0xf5, 0x87}} */

//================ UPDATE THIS TO THE CORRECT ADDR =================
static linkaddr_t light_addr =        {{0x00, 0x12, 0x4b, 0x00, 0x16, 0x65, 0xf5, 0x87}}; // modify this to change the light-sensing node
//=================================================================

static int sync_flag = FALSE;
static int req_flag = FALSE;
static int seq = 0;

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
  unsigned long in_proximity_since; //when i first receive a packet from this source
  unsigned long out_of_prox_since; //the first timestamp where it is out-of-proximity
  short rssi_values[RSSI_WINDOW];
  int rssi_ptr;
} packet_store_struct;

typedef struct {
  int data[10];
} light_data_arr;

/*========================start rolling rssi stuff========================*/
static float get_average_rssi();
static void clear_rssi_values();
static void on_absent_state();
/*---------------------------------------------------------------------------*/
static float get_average_rssi(short *rssi_values) {
  int num_valid_rssi = 0;
  int total_rssi = 0;
  
  for (int i = 0; i < RSSI_WINDOW; i++ ) {
    if (rssi_values[i] != 0) {
      num_valid_rssi += 1;
      total_rssi += rssi_values[i];
    }
  }

  return ((float) total_rssi)/num_valid_rssi;
}

static void clear_rssi_values(short *rssi_values) {
  for (int i = 0; i < RSSI_WINDOW; i++ ) {
    rssi_values[i] = 0;
  }
}

/*---------------------------------------------------------------------------*/
// duty cycle = SLOT_TIME / (SLOT_TIME + SLOT_TIME * SLEEP_CYCLE)
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

// save the previous time it discovered the neighbour
unsigned long prev_discovery_timestamp = -1;

//struct holding information about node we have connected with
static packet_store_struct sender_info;

static bool state = ABSENT;


// reinitialize some variables upon transitioning to absent_state
static void on_absent_state() {
  sender_info.src_id = -1;
  req_flag = FALSE;
  state = ABSENT;
}

// Starts the main contiki neighbour discovery process
PROCESS(nbr_discovery_process, "cc2650 neighbour discovery process");
// PROCESS(process_light_sensor, "light sensor");
AUTOSTART_PROCESSES(&nbr_discovery_process);


// Function called after reception of a packet
void receive_packet_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) 
{ 
  signed short recv_rssi = (signed short)packetbuf_attr(PACKETBUF_ATTR_RSSI);
  curr_timestamp = clock_time();
  prev_discovery_timestamp = curr_timestamp;


  if (len == sizeof(data_packet)) {
    data_packet_struct received_packet_data;
    memcpy(&received_packet_data, data, len);
    printf("Received neighbour discovery packet %lu with rssi %d from %ld\n", received_packet_data.seq, recv_rssi,received_packet_data.src_id);

    if (!sync_flag && received_packet_data.seq % 2 != 0) {
      printf("Attempting to sync\n");
      sync_flag = TRUE;
      printf("Set sync flag\n");
    }

    if (received_packet_data.src_id != sender_info.src_id) { //new sender detected
      sender_info.src_id = received_packet_data.src_id;
      sender_info.in_proximity_since = -1;
      sender_info.out_of_prox_since = -1;
      clear_rssi_values(sender_info.rssi_values);
      sender_info.rssi_ptr = 0;
    }

    sender_info.rssi_values[sender_info.rssi_ptr] = recv_rssi;
    sender_info.rssi_ptr = (sender_info.rssi_ptr + 1) % RSSI_WINDOW;

    if (get_average_rssi(sender_info.rssi_values) > -65) { //in proximity
      if (sender_info.in_proximity_since == -1) {
        sender_info.in_proximity_since = curr_timestamp;
        sender_info.out_of_prox_since = -1;
      } 

      unsigned long time_diff = curr_timestamp - sender_info.in_proximity_since;
      if (state != DETECT && time_diff/CLOCK_SECOND >= IN_PROXIMITY_THRESHOLD) {
        printf("%3lu.%03lu DETECT %ld\n", sender_info.in_proximity_since / CLOCK_SECOND, ((sender_info.in_proximity_since % CLOCK_SECOND)*1000) / CLOCK_SECOND, sender_info.src_id);
        req_flag = TRUE;
        state = DETECT;        
      } 
    } else {
      if (sender_info.out_of_prox_since == -1) {
        sender_info.out_of_prox_since = curr_timestamp;
        sender_info.in_proximity_since = -1;
      } 
      unsigned long time_diff = curr_timestamp - sender_info.out_of_prox_since;
      if (state != ABSENT && time_diff/CLOCK_SECOND >= OUT_OF_PROXIMITY_THRESHOLD) {
        printf("%3lu.%03lu ABSENT %ld\n", sender_info.out_of_prox_since / CLOCK_SECOND, ((sender_info.out_of_prox_since % CLOCK_SECOND)*1000) / CLOCK_SECOND, sender_info.src_id);
        on_absent_state();
      }       
    }

  } else {
    light_data_arr received_data;
    req_flag = FALSE; // received data, no more asking
    // Copy the content of packet into the data structure
    
    memcpy(&received_data, data, len);
    printf("Light:  ");
    for (int i = 0; i < 10; i++) {
      printf("Reading %d: %d.%02d lux, ", i+1, received_data.data[i]/100, received_data.data[i]%100);
    }
    printf("\n");
  }
}


// Scheduler function for the sending of neighbour discovery packets
char sender_scheduler(struct rtimer *t, void *ptr) {
 
  static uint16_t i = 0;
  static int sleep_counter = 0;
  static int sc;
  static int j;
  static unsigned long scheduler_curr_timestamp;
  // static int info;
  // Begin the protothread

  //take curr_time after each rtimer callback
  scheduler_curr_timestamp = clock_time();
  PT_BEGIN(&pt);


  printf("Start clock %lu ticks, timestamp %3lu.%03lu\n", scheduler_curr_timestamp, scheduler_curr_timestamp / CLOCK_SECOND, 
  ((scheduler_curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);

  start_clock_time =  scheduler_curr_timestamp;
  
  // total sleep for 0.9s, wake for 0.1s in a 1s period
  NETSTACK_RADIO.off();
  while(1){
    sc = sleep_counter % TOTAL_SLOTS; // -> 0 ~ 9 slots only.
    for (j = 0; j < sc; j++) {
      // printf(" Sleep for %d slots first before doing SEND routine\n", sc);
      rtimer_set(t, RTIMER_TIME(t) + SLOT_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
      PT_YIELD(&pt);
    }

    // radio on
    NETSTACK_RADIO.on();

    // send NUM_SEND number of neighbour discovery beacon packets
    for(i = 0; i < NUM_SEND; i++){

      // Initialize the nullnet module with information of packet to be trasnmitted
      nullnet_buf = (uint8_t *)&data_packet; //data transmitted
      nullnet_len = sizeof(data_packet); //length of data transmitted
      if (req_flag) {
        data_packet.seq = REQ;
      } else {
        data_packet.seq = seq;
        seq++;
      }

      data_packet.timestamp = scheduler_curr_timestamp;

      // printf("Send seq# %lu  @ %8lu ticks   %3lu.%03lu\n", data_packet.seq, curr_timestamp, curr_timestamp / CLOCK_SECOND, ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);
      NETSTACK_NETWORK.output(&light_addr); //Packet transmission
      
      // wait for SLOT_TIME before sending the next packet
      if(i != (NUM_SEND - 1)){
        rtimer_set(t, RTIMER_TIME(t) + SLOT_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
        PT_YIELD(&pt);
      }
  
    }

    NETSTACK_RADIO.off();
    // info = 9 - sc;
    for (j = SLEEP_CYCLE - sc; j > 0; j--) {
      // printf(" Sleep for %d slots first before going into NEXT routine\n", info);
      rtimer_set(t, RTIMER_TIME(t) + SLOT_TIME, 1, (rtimer_callback_t) sender_scheduler, ptr);
      PT_YIELD(&pt);
    }
 
    if (!sync_flag) {
      sleep_counter++; 
    }

    unsigned long time_diff = scheduler_curr_timestamp - prev_discovery_timestamp;

    if (time_diff/CLOCK_SECOND > 5) { //maybe somehow got out of sync
      sync_flag = FALSE;
    }

    //no packets received for good 30 seconds -> other device might have died
    if (time_diff/CLOCK_SECOND >= OUT_OF_PROXIMITY_THRESHOLD) { 
      if (state != ABSENT) {
        printf("%3lu.%03lu ABSENT %ld\n", prev_discovery_timestamp / CLOCK_SECOND, ((prev_discovery_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND, sender_info.src_id);        
      }
      on_absent_state();
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
  data_packet.seq = seq; //Initialize the sequence number of the packet
  
  nullnet_set_input_callback(receive_packet_callback); //initialize receiver callback
  linkaddr_copy(&dest_addr, &linkaddr_null);

  printf("CC2650 neighbour discovery\n");
  printf("Node %d will be sending packet of size %d Bytes\n", node_id, (int)sizeof(data_packet_struct));

  rtimer_set(&rt, RTIMER_NOW() + (RTIMER_SECOND / 1000), 1, (rtimer_callback_t)sender_scheduler, NULL);

  PROCESS_END();
}
