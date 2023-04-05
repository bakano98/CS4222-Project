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
#define WAKE_TIME RTIMER_SECOND/10    // 10 HZ, 0.1s
#define SLEEP_CYCLE  9        	      // 0 for never sleep
#define SLEEP_SLOT RTIMER_SECOND/10   // sleep slot should not be too large to prevent overflow

#define NUM_DATA 10 // modify this to increase the number of experiments -- minimum is 10.

// For neighbour discovery, we would like to send message to everyone. We use Broadcast address:
linkaddr_t dest_addr;

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

// Starts the main contiki neighbour discovery process
PROCESS(nbr_discovery_process, "cc2650 neighbour discovery process");
AUTOSTART_PROCESSES(&nbr_discovery_process);

// Function called after reception of a packet
void receive_packet_callback(const void *data, uint16_t len, const linkaddr_t *src, const linkaddr_t *dest) 
{


  // Check if the received packet size matches with what we expect it to be

  if(len == sizeof(data_packet)) {

 
    static data_packet_struct received_packet_data;

    curr_timestamp = clock_time();

    // gets the timestamp after offsetting the time it took to send
    unsigned long after_offset = curr_timestamp - start_clock_time;
    unsigned long diff = 0;
    // Copy the content of packet into the data structure
    memcpy(&received_packet_data, data, len);
    

    // Print the details of the received packet
    printf("========\n");
    printf("Received neighbour discovery packet %lu with rssi %d from %ld\n", received_packet_data.seq, (signed short)packetbuf_attr(PACKETBUF_ATTR_RSSI),received_packet_data.src_id);
    printf("========\n");

    printf("Current timestamp: %3lu.%03lu\n", curr_timestamp / CLOCK_SECOND, ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);
    printf("========\n");

    printf("Timestamp after accounting for offset: %3lu.%03lu\n", after_offset / CLOCK_SECOND, ((after_offset % CLOCK_SECOND)*1000) / CLOCK_SECOND);

    if (prev_discovery_timestamp != -1) {
      diff = curr_timestamp - prev_discovery_timestamp;
      printf("Time difference between current and last discovery: %3lu.%03lu\n", diff / CLOCK_SECOND, ((diff % CLOCK_SECOND)*1000) / CLOCK_SECOND);
      printf("========\n");
    } else {
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
          recv_time / CLOCK_SECOND, ((recv_time % CLOCK_SECOND)*1000) / CLOCK_SECOND, 
          prev_time / CLOCK_SECOND, ((prev_time % CLOCK_SECOND)*1000) / CLOCK_SECOND);
      }
    }
  }

}

// Scheduler function for the sender of neighbour discovery packets
char sender_scheduler(struct rtimer *t, void *ptr) {
 
  static uint16_t i = 0;
  
  static int NumSleep=0;
 
  // Begin the protothread
  PT_BEGIN(&pt);

  // Get the current time stamp
  curr_timestamp = clock_time();

  printf("Start clock %lu ticks, timestamp %3lu.%03lu\n", curr_timestamp, curr_timestamp / CLOCK_SECOND, 
  ((curr_timestamp % CLOCK_SECOND)*1000) / CLOCK_SECOND);

  start_clock_time =  curr_timestamp;
  
  while(1){

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

      NETSTACK_NETWORK.output(&dest_addr); //Packet transmission
      

      // wait for WAKE_TIME before sending the next packet
      if(i != (NUM_SEND - 1)){

        rtimer_set(t, RTIMER_TIME(t) + WAKE_TIME, 1, (rtimer_callback_t)sender_scheduler, ptr);
        PT_YIELD(&pt);
      
      }
   
    }

    // sleep for a random number of slots
    if(SLEEP_CYCLE != 0){
    
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
      for(i = 0; i < NumSleep; i++){
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

  // Start sender in one millisecond.
  rtimer_set(&rt, RTIMER_NOW() + (RTIMER_SECOND / 1000), 1, (rtimer_callback_t)sender_scheduler, NULL);

  

  PROCESS_END();
}
