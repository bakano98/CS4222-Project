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

// Identification information of the node


// Configures the wake-up timer for neighbour discovery 
#define DISCOVER_WITHIN 10*RTIMER_SECOND
#define N 13
#define SLOT_TIME DISCOVER_WITHIN/(N*N)

#define NUM_DATA 50 // modify this to increase the number of experiments -- minimum is 10.

// For neighbour discovery, we would like to send message to everyone. We use Broadcast address:
linkaddr_t dest_addr;

// To store the result of get_param
static radio_value_t value;

#define NUM_SEND 2
/*---------------------------------------------------------------------------*/
typedef struct {
  unsigned long src_id;
  unsigned long startup_time; // appends the time in which the device started up
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

// save timestamp when device started sending 1st packet
unsigned long start_first_packet = -1;

// save previous time it discovered the neighbour
unsigned long prev_discovery_timestamp = -1;

// array to store all the packets received so far -- experiment: 50 received packets then we stop
packet_store_struct storage[NUM_DATA];
static packet_store_struct current;
static unsigned int counter = 0;

// Starts the main contiki neighbour discovery process
PROCESS(nbr_discovery_process, "cc2650 neighbour discovery process");
AUTOSTART_PROCESSES(&nbr_discovery_process);

// Function called after reception of a packet
void receive_packet_callback(const void* data, uint16_t len, const linkaddr_t* src, const linkaddr_t* dest)
{


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


    /* Uncomment the following for Experiment 2. This will print out the time taken in order to receive the packet from B*/
    // unsigned long startup_B = received_packet_data.startup_time;
    // unsigned long diff_from_startup = received_packet_data.timestamp - startup_B;
    // printf("=============================================\n");
    // printf("Packet Number: %ld\n", received_packet_data.seq);
    // printf("Time taken to receive packet from startup: %3lu.%03lu\n", diff_from_startup / CLOCK_SECOND, ((diff_from_startup % CLOCK_SECOND)*1000) / CLOCK_SECOND);
    // printf("=============================================\n");

    printf("Timestamp: %3lu.%03lu\n", after_offset / CLOCK_SECOND, ((after_offset % CLOCK_SECOND) * 1000) / CLOCK_SECOND);

    if (prev_discovery_timestamp != -1) {
      diff = curr_timestamp - prev_discovery_timestamp;
      printf("%d: Time difference between current and last discovery: %3lu.%03lu\n", counter, diff / CLOCK_SECOND, ((diff % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
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
      printf("Number, Packet Number, Received Time, Interval Between Receive\n");
      unsigned long total_time = 0;
      for (int i = 0; i < NUM_DATA; i++) {
        unsigned long id = storage[i].src_id;
        unsigned long recv_time = storage[i].recv_timestamp;
        unsigned long prev_time = storage[i].prev_timestamp;
        total_time += prev_time;
        printf("   %d   ,      %lu       ,   %3lu.%03lu  ,      %3lu.%03lu    \n", i, id,
          recv_time / CLOCK_SECOND, ((recv_time % CLOCK_SECOND) * 1000) / CLOCK_SECOND,
          prev_time / CLOCK_SECOND, ((prev_time % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
      }
      total_time /= NUM_DATA - 1;
      printf("Total average interval: %3lu.%03lu\n", total_time / CLOCK_SECOND, ((total_time % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
    }
  }

}

// Scheduler function for the sender of neighbour discovery packets
char sender_scheduler(struct rtimer* t, void* ptr) {

  static int row = 0;
  static int col = 0;
  static int curr_slot = 0;

  // Begin the protothread
  PT_BEGIN(&pt);

  row = random_rand() % N;
  col = random_rand() % N;

  // Get the current time stamp
  curr_timestamp = clock_time();

  printf("Start clock %lu ticks, timestamp %3lu.%03lu\n", curr_timestamp, curr_timestamp / CLOCK_SECOND,
    ((curr_timestamp % CLOCK_SECOND) * 1000) / CLOCK_SECOND);
  if (start_first_packet == -1) {
    start_first_packet = curr_timestamp;
  }
  start_clock_time = curr_timestamp;

  while(1) {
    NETSTACK_RADIO.get_value(RADIO_PARAM_POWER_MODE, &value);
    // printf("Current network status: %s\n", value == RADIO_POWER_MODE_OFF ? "off" : "on");
    if (curr_slot % N == col || (curr_slot / N) % N == row) {
      if (value == RADIO_POWER_MODE_OFF){
        NETSTACK_RADIO.on();
      }
      nullnet_buf = (uint8_t*)&data_packet; //data transmitted
      nullnet_len = sizeof(data_packet); //length of data transmitted

      data_packet.seq++;
      data_packet.startup_time = start_first_packet;
      curr_timestamp = clock_time();

      data_packet.timestamp = curr_timestamp;
      NETSTACK_NETWORK.output(&dest_addr); //Packet transmission

      rtimer_set(t, RTIMER_TIME(t) + SLOT_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
      PT_YIELD(&pt);

      data_packet.seq++;
      data_packet.startup_time = start_first_packet;
      curr_timestamp = clock_time();

      data_packet.timestamp = curr_timestamp;
      NETSTACK_NETWORK.output(&dest_addr); //Packet transmission


    } else {
      if (value == RADIO_POWER_MODE_ON){
        NETSTACK_RADIO.off();
      }

      rtimer_set(t, RTIMER_TIME(t) + SLOT_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
      PT_YIELD(&pt);
    }

    curr_slot = (curr_slot + 1) % (N*N);
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

  // Start sender in one millisecond.
  rtimer_set(&rt, RTIMER_NOW() + (RTIMER_SECOND / 1000), 1, (rtimer_callback_t)sender_scheduler, NULL);



  PROCESS_END();
}
