/******************************
* Contiki Raft Node
* Jacob English
* je787413@ohio.edu
*******************************/

#include "contiki.h"
#include "sys/etimer.h"
#include "net/ip/uip.h"
#include "net/ipv6/uip-ds6.h"

#include "simple-udp.h"
#include "sys/timer.h"

#include "raft.h"

#include <stdio.h>
#include <stdint.h>

static struct Raft node;
static struct timer nodeTimer;
bool init = false;

static struct simple_udp_connection broadcast_connection;
uip_ipaddr_t addr;

/*---------------------------------------------------------------------------*/
PROCESS(raft_node_process, "UDP broadcast raft node process");
AUTOSTART_PROCESSES(&raft_node_process);
/*---------------------------------------------------------------------------*/
static void
receiver(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen) {
  printf("GOT MESSAGE\n");
  struct Msg *msg = (struct Msg *)data;
  msg_print(msg);

  if (msg->term > node.term) { //got msg with higer term, update term and change to follower state
    node.term = msg->term;
    raft_set_follower(&node);
  }
  else if (msg->term < node.term) { //ignore states less than node state
    return;
  }

  switch (node.state) {
    case follower:
      //election
      if (msg->type == election) {
        struct Election *elect = (struct Election *)data;
        election_print(elect);

        //reset timer
        timer_set(&nodeTimer, node.timeout * CLOCK_SECOND);

        //build vote msg
        struct Vote voteMsg;
        build_vote(&voteMsg, node.term, (uip_ipaddr_t *)sender_addr, true);

        uip_ipaddr_t nullAddr;
        uip_ipaddr(&nullAddr, 0,0,0,0);
        if (uip_ipaddr_cmp(&nullAddr, &node.votedFor)) { //vote has not been used
          //update node.votedFor to sender_addr
          uip_ipaddr_copy(&node.votedFor, sender_addr);
          //voteGranted = true already set
        }
        else { //vote was used this term
          //voteGranted = false
          voteMsg.voteGranted = false;
        }

        //send vote
        uip_create_linklocal_allnodes_mcast(&addr);
        simple_udp_sendto(&broadcast_connection, &voteMsg, sizeof(voteMsg), &addr);
      }
      //heartbeat
      if (msg->type == heartbeat) {
        struct Heartbeat *heart = (struct Heartbeat *)data;
        heartbeat_print(heart);

        //reset timer
        timer_set(&nodeTimer, node.timeout * CLOCK_SECOND);
      }
      //log stuff later on
      break;
    case candidate:
      //vote response
      if (msg->type == vote) {
        struct Vote *vote = (struct Vote *)data;
        vote_print(vote);

        //vote is for this node
        if ((uip_ipaddr_cmp(&vote->voteFor, receiver_addr)) && (vote->voteGranted)) {
          //increment vote count
          ++node.totalVotes;
          if (node.totalVotes > (TOTAL_NODES / 2)) { //if vote count is majority, change to leader & send heartbeat
            raft_set_leader(&node);
            struct Heartbeat heart;
            build_heartbeat(&heart, node.term, 0, 0, 0, 0); //fill in vars later

            uip_create_linklocal_allnodes_mcast(&addr);
            simple_udp_sendto(&broadcast_connection, &heart, sizeof(heart), &addr);
          }
          else { //else reset timer
            timer_set(&nodeTimer, node.timeout * CLOCK_SECOND);
          }
        }
      }
      break;
    case leader:
      //log stuff later on
      break;
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(raft_node_process, ev, data) {
  static struct etimer leaderTimer;

  PROCESS_BEGIN();

  printf("--BROADCAST RAFT NODE PROCESS BEGIN--\n");

  if (!init) {
    raft_init(&node);
    init = true;
  }

  raft_print(&node);

  timer_set(&nodeTimer, node.timeout * CLOCK_SECOND);

  simple_udp_register(&broadcast_connection, UDP_PORT,
                      NULL, UDP_PORT,
                      receiver);

  while(1) {
    if ((node.state == follower) || (node.state == candidate)) {
      //if timeout, update term and state and send election msg
      if (timer_expired(&nodeTimer)) {
        printf("msg timeout, starting election process\n");
        timer_set(&nodeTimer, node.timeout * CLOCK_SECOND);
        ++node.term;
        raft_set_candidate(&node);

        //send election
        struct Election elect;
        build_election(&elect, node.term, 0, 0); //Log term and index to be added later

        uip_create_linklocal_allnodes_mcast(&addr);
        simple_udp_sendto(&broadcast_connection, &elect, sizeof(elect), &addr);
      }
    }
    else if (node.state == leader) {
      //on leader timer proc wait until
      etimer_set(&leaderTimer, LEADER_SEND_INTERVAL * CLOCK_SECOND);
      PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&leaderTimer));

      //send heartbeat
      struct Heartbeat heart;
      build_heartbeat(&heart, node.term, 0, 0, 0, 0); //add rest of params later

      uip_create_linklocal_allnodes_mcast(&addr);
      simple_udp_sendto(&broadcast_connection, &heart, sizeof(heart), &addr);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
