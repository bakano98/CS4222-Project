/*
* CS4222/5422: Group Project
* Light Sensing Node. This is the node that performs the light sensing.
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
#define WAKE_TIME RTIMER_SECOND/10    // 10 HZ, 0.1s
#define SLEEP_CYCLE  9        	      // 0 for never sleep
#define SLEEP_SLOT RTIMER_SECOND/10   // sleep slot should not be too large to prevent overflow
#define SAMPLING_INTERVAL RTIMER_SECOND * 3 // 30s sampling interval

#define MAX_NODES 4 // modify to specify max number of nodes that can be in proximity
#define NUM_DATA 10 // modify this to increase the number of experiments -- minimum is 10.
#define RSSI_WINDOW 5 // the number of rssi_values we want to keep
#define IN_PROXIMITY_THRESHOLD 10
#define OUT_OF_PROXIMITY_THRESHOLD 10
#define REQ 12345678


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
// custom struct to store information per for each node that are in sync...
typedef struct {
  unsigned long src_id;
  unsigned long in_proximity_since; //when i first receive a packet from this source
  unsigned long out_of_prox_since; //the first timestamp where it is out-of-proximity
  unsigned long prev_discovery_time;
  short rssi_values[RSSI_WINDOW];
  int rssi_ptr;
  bool state;
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
static struct pt light_pt;

// Structure holding the data to be transmitted
static data_packet_struct data_packet;

// Current time stamp of the node
unsigned long curr_timestamp;

// save timestamp when it started sending
unsigned long start_clock_time;


// array to store sampling data
static int light_data[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int light_data_counter = 0;

// neighbours discovered so far. we allow discovery of up to 5 neighbours
packet_store_struct node_tracker[MAX_NODES];
static bool node_mem_map[MAX_NODES];


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
static float get_average_rssi();
static void clear_rssi_values();
static void clear_node_mem_map();
/*---------------------------------------------------------------------------*/

static void clear_node_mem_map() {
  for (int i = 0; i < MAX_NODES; i++) {
    node_mem_map[i] = FALSE;
  }
}

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
  // printf("Sending to address: ");
  // LOG_INFO_LLADDR(dest);
  // printf("\n");
  light_data_arr temp;
  for (int i = 0; i < 10; i++) {
    temp.data[i] = light_data[i];
  }
  nullnet_buf = (uint8_t *)&temp;
  nullnet_len = sizeof(temp);
  NETSTACK_NETWORK.output(dest);
  // printf("Light: ");
  // for (int i = 0; i < 10; i++) {
  //   printf("Reading %d = %d.%02d LUX ", i+1, light_data[i]/100, light_data[i]%100);
  // }
  // printf("\n");
}

/*========================end light sensor stuff========================*/


// Function called after reception of a packet
void receive_packet_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) 
{ 
  // Check if the received packet size matches with what we expect it to be
  if(len == sizeof(data_packet)) {
    static data_packet_struct received_packet_data;
    int i;
    int req_flag = FALSE;
    curr_timestamp = clock_time();

    // Copy the content of packet into the data structure
    memcpy(&received_packet_data, data, len);
    signed short recv_rssi = (signed short)packetbuf_attr(PACKETBUF_ATTR_RSSI);
    printf("Received neighbour discovery packet %lu with rssi %d from %ld\n", received_packet_data.seq, recv_rssi,received_packet_data.src_id);

    if (received_packet_data.seq == REQ) {
      printf("Received REQ\n");
      req_flag = TRUE;
    }


    packet_store_struct *curr = NULL;

    static int node_slot = -1;
    for (i = 0; i < MAX_NODES; i++) {
      if (node_tracker[i].src_id == received_packet_data.src_id && node_mem_map[i] == TRUE) {
        //node previously registered
        curr = &node_tracker[i];
        node_slot = i;
        break;
      }

      if (node_mem_map[i] == FALSE && node_slot == -1) {
        node_slot = i;
      }

    }
    
    if (curr == NULL && node_slot == -1) {
      printf("Max capacity reached\n");
    } else {

      if (curr == NULL) { //register new node by initializing node_tracker slot
        curr = &node_tracker[node_slot];
        node_mem_map[node_slot] = TRUE;
        
        curr->in_proximity_since = -1;
        curr->out_of_prox_since = -1;
        curr->state = ABSENT;
        curr->rssi_ptr = 0;
        curr->src_id = received_packet_data.src_id;

        clear_rssi_values(curr->rssi_values);
      }


      //perform action on registered node
      curr->prev_discovery_time = curr_timestamp;
      curr->rssi_values[curr->rssi_ptr] = recv_rssi;
      curr->rssi_ptr = (curr->rssi_ptr + 1) % RSSI_WINDOW;
      
      if (get_average_rssi(curr->rssi_values) > -65) {
        if (curr->in_proximity_since == -1) {
          curr->in_proximity_since = curr_timestamp;
          curr->out_of_prox_since = -1;
        } 

        unsigned long time_diff = curr_timestamp - curr->in_proximity_since;
        if (curr->state != DETECT && time_diff/CLOCK_SECOND >= IN_PROXIMITY_THRESHOLD) {
          printf("%3lu.%03lu DETECT %ld\n", curr->in_proximity_since / CLOCK_SECOND, ((curr->in_proximity_since % CLOCK_SECOND)*1000) / CLOCK_SECOND, curr->src_id);
          // keep sending while data is being requested from master
          curr->state = DETECT;
        } 

      } else {
        if (curr->out_of_prox_since == -1) {
          curr->out_of_prox_since = curr_timestamp;
          curr->in_proximity_since = -1;
        } 

        unsigned long time_diff = curr_timestamp - curr->out_of_prox_since;
        if (curr->state != ABSENT && time_diff/CLOCK_SECOND >= OUT_OF_PROXIMITY_THRESHOLD) {
          printf("%3lu.%03lu ABSENT %ld\n", curr->out_of_prox_since / CLOCK_SECOND, ((curr->out_of_prox_since % CLOCK_SECOND)*1000) / CLOCK_SECOND, curr->src_id);
          curr->state = ABSENT;
          node_mem_map[node_slot] = FALSE;
        } 

      }

      if (curr->state == DETECT && req_flag) {
        send_light_data(src);
      }
      
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
      nullnet_buf = (uint8_t *)&data_packet; //data transmitted
      nullnet_len = sizeof(data_packet); //length of data transmitted
      data_packet.seq++;
      curr_timestamp = clock_time();
      data_packet.timestamp = curr_timestamp;

      NETSTACK_NETWORK.output(&dest_addr); //Packet transmission
      
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

    for (i = 0; i < MAX_NODES; i++) {
      if (node_mem_map[i]) {
        unsigned long time_diff = (curr_timestamp - node_tracker[i].prev_discovery_time)/CLOCK_SECOND;
        if (time_diff >= OUT_OF_PROXIMITY_THRESHOLD) {
          if (node_tracker[i].state != ABSENT) {
            printf("%3lu.%03lu ABSENT %ld\n", node_tracker[i].prev_discovery_time / CLOCK_SECOND, ((node_tracker[i].prev_discovery_time % CLOCK_SECOND)*1000) / CLOCK_SECOND, node_tracker[i].src_id);
            node_tracker[i].state = ABSENT;
          }
          node_mem_map[i] = FALSE;
        }
      }
    }


    if (prev_sample_time == -1) {
      // printf("Collecting first light reading\n");
      get_light_reading();
      prev_sample_time = RTIMER_NOW();
    }

    if (RTIMER_NOW() - prev_sample_time  >= SAMPLING_INTERVAL) {
      // printf("Interval reached, collecting reading\n");
      get_light_reading();
      prev_sample_time = RTIMER_NOW();
    }

  }

  PT_END(&light_pt);
}


// Main thread that handles the neighbour discovery process
PROCESS_THREAD(nbr_discovery_process, ev, data)
{

 // static struct etimer periodic_timer;

  PROCESS_BEGIN();
  clear_node_mem_map();
  data_packet.src_id = node_id; //Initialize the node ID
  data_packet.seq = 0; //Initialize the sequence number of the packet
  
  nullnet_set_input_callback(receive_packet_callback); //initialize receiver callback
  linkaddr_copy(&dest_addr, &linkaddr_null); 

  // if it is the light sensor node, keep collecting data very SAMPLING_INTERVAL
  if (linkaddr_cmp(&light_addr, &linkaddr_node_addr)) {
    printf("============ This is the light sensing node ============\n\n");
    init_opt_reading();
    rtimer_set(&rt, RTIMER_NOW() + RTIMER_SECOND, 1,  (rtimer_callback_t)schedule_sleep, NULL);
  } else {
    printf("============ ERROR ============\n\n");
    printf("You have flashed this device as the light sensing node\n\n");
    printf("but you have not changed the light_addr in line46 to  \n\n");
    printf("this devices's IP address \n\n");

    printf("Please also check that you have changed the ip address \n\n");
    printf("in nbr-part2-requester.c to use this device's IP address \n\n");
  }

  PROCESS_END();
}

