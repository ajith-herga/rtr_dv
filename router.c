/* client_crc_filewrite.c
 * CS423-MP2
 * NETID: herga2, rmohiud2
 * The file which implements all the functionality required in the MP2
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <linux/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <search.h>
#include <math.h>
#include <poll.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#ifdef DEBUG
#define Dlog(...) printf(__VA_ARGS__)
#else
#define Dlog(...)
#endif
#define PORT "3001" // the port client will be connecting to 

#define MAXDATASIZE 8802 // max number of bytes we can get at once
                         // adjusted for one additional null character 
struct pollfd fdinfo[2] = {0};
int send_sockfd = 0;
int send_routing_update = 0;
uint16_t last_rcv_src_advt = 0;
int state_manager = 0;
uint16_t udp_port = 0;
uint16_t  my_addr = 0;
uint16_t count_rtb = 0;
uint8_t tcp_killed = 0;
uint8_t log_on_sent = 0;
enum {
    MY_ADDR = 0,
    NEIGH_END,
    LINK_END
};
enum {
    MANAGER = 1,
    ROUTER,
    ADVT
};
enum {
    NEIGH,
    ADDR,
    LOCALHOST,
    UDP_PORT,
    COST
};
enum {
    LINK_COST,
    NODE1,
    NODE2,
    NEW_COST
};

struct node *rtb = NULL;
struct node *ltb = NULL;
struct packet_gen {
uint8_t opcode;
uint16_t dest;
uint16_t src;
char buf[8762];
} __attribute__ ((packed));

struct packet_mngr {
uint8_t opcode;
uint16_t dest;
char buf[8778];
} __attribute__ ((packed));

char hostname[100];

struct node {
    struct node *next;
    uint16_t data;
    int16_t cost;
    char *host;
    uint16_t conn_addr;
    uint16_t conn_port;
};

struct node *
new_route (uint16_t data, uint16_t port, int16_t cost, char *buf, int len_buf, uint16_t connect_addr)
{
    struct node *temp = (struct node *)malloc(sizeof(struct node));
    temp->data = data;
    temp->cost = cost;
    temp->host = (char *)malloc((len_buf + 1)*sizeof(char));
    memcpy(temp->host, buf, len_buf);
    temp->host[len_buf] = '\0';
    temp->conn_addr = connect_addr;
    temp->conn_port = port;
    temp->next = NULL;
    return temp;
}

void
remove_node (struct node *temp)
{
    free(temp->host);
    free(temp);
}

int
tx_generic_udp_send_pkt (char *hostname, struct packet_gen *buf, uint16_t port, int length)
{
	struct addrinfo hints, *servinfo, *p;
	int rv, numbytes, sockfd;
    char port_string[10];
    char local_hostname[100] = {0};
    memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
    snprintf(port_string, 6, "%d", port);
    if (strncmp(hostname, "local", 5) == 0) {
         if(gethostname(local_hostname, 100)) {
             perror("Could not get hostname\n");
             exit(EXIT_FAILURE);
         }
         /* Using ews hostname to subvert issues with udp rx on localhost
          * Original hostname is not changed, the local pointer is pointed to
          * ews version.
          */
         hostname = local_hostname;
    }
	if ((rv = getaddrinfo(hostname, port_string, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and make a socket
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("tx_generic_udp_send_pkt: No socket");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "tx_generic_udp_send_pkt: failed to bind socket\n");
		return 2;
	}

	if ((numbytes = sendto(sockfd, buf, length, 0,
			 p->ai_addr, p->ai_addrlen)) == -1) {
		perror("tx_generic_udp_send_pkt: sendto");
		exit(1);
	}

	freeaddrinfo(servinfo);

	Dlog("tx_generic_udp_send_pkt: sent %d bytes to %s:%d port %d\n", numbytes, hostname, port, sockfd);
	close(sockfd);
    return numbytes;
}

/*
 * Its assumed that the char *buf is /0 terminated
 */
void
push (struct node **head, uint16_t  data,uint16_t port, int16_t cost, char *buf, uint16_t connect_addr)
{
    struct node *temp = NULL, *save = NULL;
    if (!head) return;
    temp = new_route(data, port, cost, buf, strlen(buf), connect_addr);
    save = *head;
    *head = temp;
    temp->next = save;
    return;
}

int
size_pkt_gen (int len) {

    return (sizeof(struct packet_gen) - 8762 + len + 10);
}

/* This function looks for connected links, to send updates on routing to them
 */
void
get_connected_ports (struct node *temp)
{
    struct node *current = NULL;
    int update_size = 0, min_size = size_pkt_gen(0), increment_size, numbytes;
    struct packet_gen upd_pkt = {0};
    char *buf_index = upd_pkt.buf;
    char *buf = buf_index;
    int16_t cost;
    int update_sent = 0;

    if (!temp) {
        return;
    }
    if (temp->data == last_rcv_src_advt) {
        /* Do not send if  link is NULL or the link being considered is the 
         * link from where we got this update from.
         */
        Dlog("\nSkipping ADVT to the src of last advt, %d", last_rcv_src_advt);
        return;
    }
    update_size = min_size;
    if ((temp->data == temp->conn_addr) && (temp->cost != -1)) {
        /*
         * This is a link which is connected.
         */
        Dlog("\n Considering link with %s addr %d", temp->host,  temp->conn_addr);
        current = rtb;
        upd_pkt.opcode = 3;
        upd_pkt.dest = htons(temp->conn_addr);
        upd_pkt.src = htons(my_addr);
        while (current)
        {
            if ((current->conn_addr != temp->conn_addr)) {
               if (update_size > (MAXDATASIZE + 2*increment_size)) {
                    Dlog("\nSend: %s size %d", upd_pkt.buf, update_size);
                    numbytes = tx_generic_udp_send_pkt(temp->host, &upd_pkt, temp->conn_port, update_size);
                    Dlog("\nSent Routing update %d bytes to %d tried %d", numbytes, temp->conn_addr, update_size);
                    buf_index = upd_pkt.buf;
                    buf = buf_index;
                    update_sent = 1;
                }
                /* 
                 * Spare burden of cost calculation at the destination
                 */
                if (current->cost != -1) {
                    cost = temp->cost + current->cost;
                } else {
                    /*
                     * Inf + inf = Inf
                     */
                    cost = -1;
                }
                /*
                 * Sending End: The udp_port of this router is how one can reach this advertised address, the src is
                 * picked from the src field of the packet, at the recieving end.
                 */
                snprintf(buf_index, MAXDATASIZE, "NEIGH %d %s %d %d\n", current->data, current->host,
                         udp_port, cost);
                update_size = min_size + strlen(buf);
                increment_size = strlen(buf_index);
                buf_index = buf + strlen(buf);
            }
            current = current->next;
        }

        if (update_size != min_size) {
            numbytes = update_size;
            numbytes = tx_generic_udp_send_pkt(temp->host, &upd_pkt, temp->conn_port, update_size);
            Dlog("\nSend: %s", upd_pkt.buf);
            Dlog("\nSent Routing update %d bytes to %d tried %d", numbytes, temp->conn_addr, update_size);
            buf_index = upd_pkt.buf;
            buf = buf_index;
            update_sent = 1;
        }

        if (update_sent == 1) {
            /* Rewriting the upd_pkt to hold the DONE message*/
            buf = upd_pkt.buf;
            snprintf(buf, MAXDATASIZE, "DONE_ADVERTISEMENT\n");
            numbytes = tx_generic_udp_send_pkt(temp->host, &upd_pkt, temp->conn_port, min_size + strlen(buf));
            update_sent = 0;
        }
    }
}

void
parse_all (struct node **head, void parse_function(struct node *)) {

    struct node *current = NULL;
    current = *head;
    while (current)
    {
        parse_function(current);
        current = current->next;
    }
}

int
check_count (struct node **head) {

    int count;
    struct node *current = NULL;
    current = *head;
    while (current)
    {
        count++;
        current = current->next;
    }
    if (count > count_rtb) {
        count_rtb = count;
        return 1;
    } else if (count == count_rtb) {
        return 0;
    } else {
        /* can implement a timer to delete the entries which are not neighbors, and
         * reduce the count_rtb.
         */
        Dlog ("\n Exception, router table size has reduced");
        return 0;
    }
}


void
print_all (struct node **head) {

    struct node *current = NULL;
    current = *head;
    #ifndef DEBUG
    return;
    #endif
    Dlog ("\n Attempting to printall");
    while (current)
    {
        Dlog("\n %d %s %d %d %d", current->data, current->host, current->conn_port, current->cost, current->conn_addr);
        current = current->next;
    }
}

/* Get the link cost stored and updated by the manger for a given link opposite address
 * This can change in the routing table, thus need to refer link table everytime before
 * making changes in the routing table for this address. If address not a link, we return
 * a -1 on all the queries
 */
void
get_link_cost (struct node **head, int check_addr, uint16_t *cost, uint16_t *addr, uint16_t *port)
{
    struct node *current = NULL;
    current = *head;
    *port = -1, *cost = -1, *addr = -1;
    while (current)
    {
        if (current->data == check_addr) {
           *port = current->conn_port;
           *cost = current->cost;
           *addr = current->conn_addr;
           return;
        }
        current = current->next;
    }
}


void
update_route (struct node **head, struct node *element, int16_t change) {

    struct node *current = NULL;
    int16_t link_cost = 0, link_opp = 0, link_port = 0, new_cost;
    current = *head;
    while (current)
    {
        /* 
         * Skip the element for which there has been a link change by manager
         * Just update the rest of the routes which have happened t0 be learnt
         * from the ADDR for which the link cost has changed.
         * The other routes however can be accross directly connected links. In
         * this case need to update the cost based on best reacheability.
         */
        if ((element->data == current->conn_addr) && (element->data != current->data)) {
           get_link_cost(&ltb, current->data, &link_cost, &link_opp, &link_port);
           if (element->cost == -1) {
                if (link_cost != -1) {
                    /* we have found that the address is reachable from our 
                     * link table and is connected. Thus we revert the cost
                     * to original link cost of connected network.
                     */
                    current->cost = link_cost;
                    current->conn_addr = link_opp;
                    current->conn_port = link_port;
                } else {
                    current->cost = -1;
                }
            } else {
                new_cost = current->cost + change;
                if (link_cost == -1 || link_cost >= new_cost) {
                    current->cost = new_cost;
                } else {
                    /* This is the case of updating the reachability 
                     * of the address that is connected directly via
                     * a link. We need to be careful to not mark it 
                     * at a higher cost than directly connected link
                     */
                    current->cost = link_cost;
                    current->conn_addr = link_opp;
                    current->conn_port = link_port;
                }
            }
        }
        current = current->next;
    }
}

void
update_link_cost_ltb (struct node **head, uint16_t addr, int16_t cost) {
    
    int16_t change = 0;
    struct node *current = NULL;
    current = *head;
    while (current)
    {   
        if (current->data == addr && current->conn_addr == addr) {
            current->cost = cost;
        }
        current = current->next;
    }
}


void
update_link_cost_rtb (struct node **head, uint16_t addr, int16_t cost) {
    
    int16_t change = 0;
    struct node *current = NULL;
    current = *head;
    while (current)
    {   
        if ((current->data == addr) && (current->conn_addr == addr) &&
               (current->cost != cost)) {
            change = cost - current->cost;
            current->cost = cost;
            /* Routing table for a link has been updated by the manager.
             * need to 1. send update 2. send it to all links
             */
            update_route(head, current, change);
            send_routing_update = 1;
            last_rcv_src_advt = 0;
        }
        current = current->next;
    }
}

void
delete_all (struct node **head) {

    struct node *current = NULL, *save = NULL;
    current = *head; *head = NULL;
    while (!current)
    {
        save = current->next;
        remove_node(current);
        current = save;
    }
}

int
is_equal_addr (struct node *first, struct node *second)
{
    return(first->data == second->data);
}

int
is_greater_addr (struct node *first, struct node *second)
{
    return (first->data > second->data);
}


struct node *
get_element (struct node **head, struct node *hint) {

    struct node *current = NULL;
    current = *head;
    while (current)
    {
        if (is_equal_addr(current, hint))
           return current;
        current = current->next;
    }
    return NULL;
}

void
transmit_fd (int socket, char *buf, int len)
{
    if (send(socket, buf, len, 0) == -1) {
             perror("client: OK send");
             exit(1);
    }
}


int
send_rtr_addr (struct packet_gen *data, int len, char *buf) {
    int numbytes = 0;
    struct node hint = {0}, *element;
    uint16_t addr = ntohs(data->dest);
    hint.data = addr;
    element = get_element(&rtb, &hint);
    if (!element || element->cost == -1) {
        snprintf(buf, MAXDATASIZE, "DROP %s\n", data->buf);
        Dlog("\nTo manager: %s\n", buf);
        transmit_fd(fdinfo[0].fd, buf, strlen(buf));
        return;
    } else {
        snprintf(buf, MAXDATASIZE, "LOG FWD %d %s\n", addr, data->buf);
        Dlog("\nTo manager: %s\n", buf);
        transmit_fd(fdinfo[0].fd, buf, strlen(buf));
    }

    numbytes = tx_generic_udp_send_pkt(element->host, data, element->conn_port, len);
    Dlog("\n Sent %d bytes to %d tried %d", numbytes, element->conn_port, len);
    return 1;
}
/* A dummy is inserted in the beginning for making smooth transitions
 * while adding in the front. Two pointers are used as we need to add 
 * data in the front of the bigger number. Head is adjusted in case dummy
 * is changed.
 */
void
sorted_insert_internal (struct node **head, struct node* current)
{
    struct node dummy = {0}, *pp = NULL, *p = NULL;
    int found = 0, count = 0;
    
    dummy.next = *head;
    pp = &dummy;
    p  = dummy.next; 
    while (p) {
        if (is_greater_addr(p,current)) {
            pp->next = current;
            pp->next->next = p;
            found = 1;
            break;
        }
        pp = p;
        p = p->next;
        count++;
    }
    if (found == 0) {
        pp->next = current;
    }
    *head = dummy.next;
    return;
}

/* Insert a data where its supposed to be. _internal would be called after forming 
 * a node for the data. _internal is reused in insertion sort.
 */
void
sorted_insert (struct node **head,uint16_t data, uint16_t port, int16_t cost, char *buf, uint16_t connect_addr)
{
    struct node *current = new_route(data, port, cost, buf, strlen(buf), connect_addr);
    sorted_insert_internal(head, current);
}

/*
 * we can guarantee that delete occurs just after stay in the order of insertion
 * refer to sorted_insert
*/
void
adjust_for_dup (struct node *delete, struct node *stay) {
    
    if (delete->cost == stay->cost)
        return;
    if (delete->cost > 0) {
        if ((delete->cost < stay->cost)  || (stay->cost == -1)) {
            stay->cost = delete->cost;
            stay->conn_port = delete->conn_port;
            stay->conn_addr = delete->conn_addr;
            send_routing_update = 1;
            return;
        }
    }

    if (delete->cost == -1) {
        if (stay->conn_addr != delete->conn_addr) {
            send_routing_update = -1;
            return;
        } else {
            if (stay->cost != -1) {
                send_routing_update = 1;
                stay->cost = -1;
            }
        }
    }
    return;
}

int
remove_dup (struct node *head) {

    struct node *p, *pp;
    int ret = 0;
    if (!head) {
        return;
    }
    pp = head;
    p = head->next;
    while(p) {
        while (p && (p->data == pp->data)) {
            adjust_for_dup(p,pp);
            ret++;
            pp->next = pp->next->next;
            remove_node(p);
            p = pp->next;
        }
        if (!p) {
            break;
        }
        p = p->next;
        pp = pp->next;
    }
    return ret;
}

void
sorted_insert_link (struct node **head, uint16_t data, uint16_t port, int16_t cost, char *buf, uint16_t connect_addr)
{
    int16_t link_cost, link_opp, link_port;
    if (connect_addr != 0) {
         get_link_cost(&ltb, data, &link_cost, &link_opp, &link_port);
         if (link_cost != -1) {
             sorted_insert(&rtb, data, link_port, link_cost, buf, link_opp);
         }
         return;
    }
    sorted_insert(head, data, port, cost, buf, data);
}

void
sorted_insert_neigh (struct node **head,uint16_t data, uint16_t port, int16_t cost, char *buf, uint16_t connect_addr)
{
    if (connect_addr == 0) {
        connect_addr = data;
    }
    sorted_insert(head, data, port, cost, buf, connect_addr);
}

/* IMPORTANT working of insertion sort. ability to insert a node in the right position is given.
 * this is is used to run till the end of the same list and inserting each card in the right place
 * of a new list.
 */
void
sort_list (struct node **head)
{
    struct node *save = NULL, *current = *head;
    *head = NULL;

    while (current) {
        save = current->next;
        current->next = NULL;
        sorted_insert_internal(head, current);
        current = save;
    }
}

int16_t 
update_link_cost_mngr (char *buf)
{
    char *rest_word = NULL, *word = NULL;
    int16_t new_cost = 0, addr2 = 0, addr1 = 0, data, state = LINK_COST;
    int16_t link_cost, link_opp, link_port;
    word = strtok_r(buf, " ", &rest_word);
    while (word != NULL) {
        switch (state) {
            case LINK_COST:
               state++;
               break;
            case NODE1:
               addr1 = atoi(word);
               state ++;
               break;
            case NODE2:
               addr2 = atoi(word);
               state++;
               break;
            case NEW_COST:
               new_cost = atoi(word);
               state++;
               break;
            default:
               Dlog("\nWrong state reached");
               break;
        }
        word = strtok_r(NULL, " ", &rest_word);
    }
    Dlog("\nBefore updating link cost");
    print_all(&ltb);
    print_all(&rtb);
    if (addr1 == my_addr) {
        update_link_cost_ltb(&ltb, addr2, new_cost);
        update_link_cost_rtb(&rtb, addr2, new_cost);
        data = addr2;
    } else if (addr2 == my_addr) {
        update_link_cost_ltb(&ltb, addr1, new_cost);
        update_link_cost_rtb(&rtb, addr1, new_cost);
        data = addr1;
    } else {
       Dlog("\nLink change %d %d doesnot include my_addr %d Exiting on error", addr1, addr2, my_addr);
       exit(0);
    }
    get_link_cost(&ltb, data, &link_cost, &link_opp, &link_port);
    if (link_cost != -1) {
         sorted_insert(&rtb, data, link_port, link_cost, "localhost", link_opp);
    }
    Dlog("\nAfter updating link cost");
    print_all(&ltb);
    print_all(&rtb);
    return new_cost;
}

void
neighbor_update_rtb (char *buf, uint16_t src)
{

    char *local_host = NULL, *rest_word = NULL, *word = NULL;
    uint16_t udpport = 0, addr = 0, state = NEIGH;
    int16_t cost;
    word = strtok_r(buf, " ", &rest_word);
    while (word != NULL) {
        switch (state) {
            case NEIGH:
               state++;
               break;
            case ADDR:
               addr = atoi(word);
               state ++;
               break;
            case LOCALHOST:
               local_host = word;
               state++;
               break;
            case UDP_PORT:
               udpport = atoi(word);
               state++;
               break;
            case COST:
               cost = atoi(word);
               state++;
               break;
            default:
               Dlog("\nWrong state reached");
               break;
        }
        word = strtok_r(NULL, " ", &rest_word);
    }
    Dlog("\nParsed the following: %d %s %d %d", addr, local_host, udpport, cost);
    sorted_insert_neigh(&rtb, addr, udpport, cost, local_host, src);
    /*
     * Insert link costs to the link table when manager (src = 0) talks about it.
     * Reinsert the link costs to the routing table if alternate cost has just been
     * inserted by the sorted_insert_neigh() call above.
     */
    sorted_insert_link(&ltb, addr, udpport, cost, local_host, src);
}


void
process_input_tcp (int file)
{
    char *line = NULL, *rest_line = NULL;
    char buf[MAXDATASIZE] = {0};
    char line_stack[200] = {0};
    int i, len = recv(file, buf, MAXDATASIZE, /*flags*/0);
    if (len == 0) {
        /*Think about closing fd, and removing from the polling*/
        close(file);
        return;
    }
    Dlog("\nTCP: Client Received:");
    #ifdef DEBUG_PUT
    for (i = 0; i < len; i++) {
        putchar(buf[i]);
    }
    #endif
    buf[len] = '\0';
    Dlog("\n");
    line = strtok_r(buf, "\n", &rest_line);
    while (line != NULL) {
        memcpy(line_stack, line, strlen(line));
        line_stack[strlen(line)] = '\0';
        switch(state_manager) {
            case MY_ADDR:
                if (strncmp(line_stack, "ADDR", 4) == 0) {
                    int d = atoi((char *)(line_stack + 4));
                    Dlog("\nFrom Manager ADDR: %d\n", d);
                    my_addr = d;
                    snprintf(line_stack, MAXDATASIZE, "HOST localhost %d\n", udp_port);
                    Dlog("\nTo Manager %s", line_stack);
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                }
                if (strncmp(line_stack, "OK", 2) == 0) {
                    Dlog("\nFrom Manager: OK");
                    snprintf(line_stack, MAXDATASIZE, "NEIGH?\n");
                    Dlog("\nTo Manager %s", line_stack);
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                }
                if (strncmp(line_stack, "NEIGH ", 6) == 0) {
                   neighbor_update_rtb(line_stack, 0);
                     /* 
                     * The routing update got from manager, need to send advt to all
                     * ports.
                     */
                    last_rcv_src_advt = 0;
                    check_count(&rtb);
                    remove_dup(rtb);
                    if (check_count(&rtb)) {
                        Dlog("\nCOUNT increase, send the routing update");
                        send_routing_update = 1;
                    }
                    parse_all(&rtb, get_connected_ports);
                }
                if (strncmp(line_stack, "DONE", 4) == 0) {
                    Dlog("\nFrom Manager: DONE: %d", count_rtb);
                    snprintf(line_stack, MAXDATASIZE, "READY\n");
                    Dlog("\nTo manager: %s", line_stack);
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                    state_manager++;
                }
                break;
            case NEIGH_END:
                if (strncmp(line_stack, "OK", 2) == 0) {
                    Dlog("\nFrom Manager: OK");
                    if (!log_on_sent) {
                        snprintf(line_stack, MAXDATASIZE, "LOG ON\n");
                        Dlog("\nTo Manager: %s", line_stack);
                        transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                        print_all(&rtb);
                        log_on_sent = 1;
                    }
                }
                if (strncmp(line_stack, "END", 3) == 0) {
                    Dlog("\nmanager replied with END, We are done\n");
                    exit(EXIT_SUCCESS);
                }
                if (strncmp(line_stack, "LINKCOST", 8) == 0) {
                    Dlog("\nFrom Manager: LINKCOST");
                    snprintf(line_stack, MAXDATASIZE, "COST %d OK\n", update_link_cost_mngr(line_stack));
                    Dlog("\nTo Manager: %s", line_stack);
                    if (send_routing_update) {
                        remove_dup(rtb);
                        parse_all(&rtb, get_connected_ports);
                        send_routing_update = 0;
                    }
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                    state_manager++;
                }
                break;
            case LINK_END:
                 if (strncmp(line_stack, "LINKCOST", 8) == 0) {
                    Dlog("\nFrom Manager: LINKCOST");
                    snprintf(line_stack, MAXDATASIZE, "COST %d OK\n", update_link_cost_mngr(line_stack));
                    if (send_routing_update) {
                        remove_dup(rtb);
                        parse_all(&rtb, get_connected_ports);
                        send_routing_update = 0;
                    }
                    Dlog("\nTo Manager: %s", line_stack);
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                }
                if (strncmp(line_stack, "END", 3) == 0) {
                   Dlog("\nFrom Manager: END");
                    snprintf(line_stack, MAXDATASIZE, "BYE\n");
                    Dlog("\nTo Manager: %s", line_stack);
                    transmit_fd(fdinfo[0].fd, line_stack, strlen(line_stack));
                    sleep(3);
                    exit(EXIT_SUCCESS);
                }
            default:
                break;
        }
        line = strtok_r(NULL, "\n", &rest_line);
    }
    return;
}

void
process_send_multihop_udp (struct packet_gen *packet)
{

    struct packet_gen pkt = {0};
    uint16_t dest;
    int len;
    char *buf;
 
    pkt.dest = packet->dest;
    pkt.src = packet->src;
    pkt.opcode = packet->opcode;
    dest = ntohs(packet->dest);
    len = strlen(packet->buf);

    snprintf(pkt.buf, len + 1, "%s", packet->buf);
    Dlog("\nUDP FORWARD: dest: %d", dest);
    buf = (char *)packet;
    send_rtr_addr(&pkt, size_pkt_gen(len), buf);

   return;
}

void
process_mngr_udp (char *buf) {

    struct packet_mngr *packet = (struct packet_mngr *)buf;
    struct packet_gen pkt = {0};
    uint16_t dest = ntohs(packet->dest);
    int len = strlen(packet->buf);
    
    snprintf(pkt.buf, len + 1, "%s", packet->buf);
    Dlog(" dest: %d len string %d", dest, len);

    pkt.opcode = 2;
    pkt.dest = htons(dest);
    pkt.src = htons(my_addr);
    send_rtr_addr(&pkt, size_pkt_gen(len), buf);
}

void
process_rtr_udp (char *buf) {

    struct packet_gen *packet = (struct packet_gen *)buf;
    uint16_t dest = ntohs(packet->dest);
    uint16_t src = ntohs(packet->src);
    int len = strlen(packet->buf);
    char *msg = NULL;
    Dlog("\nUDP: DATA PACKET src: %d dest: %d", src, dest);
    if (dest != my_addr) {
        process_send_multihop_udp(packet);
        return;
    }
    msg = (char *)malloc(len + 1);
    snprintf(msg, len + 1, "%s", packet->buf);
    snprintf(buf, MAXDATASIZE, "RECEIVED %s\n", msg);
    Dlog("\nTo Manager %s", buf);
    transmit_fd(fdinfo[0].fd, buf, strlen(buf));
    free(msg);
}

void
process_advt_udp (char *buf) {

    struct packet_gen *packet = (struct packet_gen *)buf;
    uint16_t dest = ntohs(packet->dest);
    char line_stack[200] = {0};
    char *line = NULL, *rest_line = NULL;
    uint16_t src = ntohs(packet->src);
    int len = strlen(packet->buf);
    Dlog("src: %d dest: %d len string %d", src, dest, len);
    if (dest != my_addr) {
        Dlog("\n The update was not sent to us");
        return;
    }
    line = strtok_r(packet->buf, "\n", &rest_line);
    while (line != NULL) {
        memcpy(line_stack, line, strlen(line));
        line_stack[strlen(line)] = '\0';
        if (strncmp(line_stack, "NEIGH ", 6) == 0) {
            neighbor_update_rtb(line_stack, src);
        }
        if (strncmp(line_stack, "DONE_ADVE", 9) == 0) {
            /* update the src from which update was recieved, no need to send ramifications
             * of this update to thw src.
             */
            last_rcv_src_advt = src;
            /*
             * Compress the routing table, by calculating the correct new
             * costs
             */
            Dlog("\nBefore compressing, after Advertisement");
            print_all(&rtb);
            remove_dup(rtb);
            Dlog("\nAfter compressing, after Advertisement");
            print_all(&rtb);
            /* If the advertisement has added new entries, need to send update to the rest
             */
            if (check_count(&rtb)) {
                Dlog("\nCOUNT increase, send the routing update");
                send_routing_update = 1;
            }
            /*
             * Global variable denotes change in routing table, send routing
             * table to all neighbors.
             */
           if (send_routing_update) {
                parse_all(&rtb, get_connected_ports);
                send_routing_update = 0;
            } else {
                Dlog("\n Got advertisement, do not need to send update");
            }
        }
        line = strtok_r(NULL, "\n", &rest_line);
    }
}

void
process_input_udp (int file) {

    char buf[MAXDATASIZE] = {0};
    struct packet_mngr *packet;
    uint8_t opcode, numbytes;
 	socklen_t addr_len;
    struct sockaddr_storage their_addr;
    int i;
	   
 	addr_len = sizeof their_addr;
	if ((numbytes = recvfrom(file, buf, MAXDATASIZE-1 , 0,
		(struct sockaddr *)&their_addr, &addr_len)) == -1) {
		perror("recvfrom");
		exit(1);
	}
    if (numbytes == 0) {
        Dlog("\n Returning");
    }
	Dlog("listener: packet is %d bytes long\n", numbytes);
    Dlog("\nUDP: Client Received:");
    #ifdef DEBUG_PUT
    for (i = 0; i < numbytes; i++) {
        putchar(buf[i]);
    }
    #endif
    buf[numbytes] = '\0';
    Dlog("\n");
 
    packet = (struct packet_mngr *)buf;
    opcode = packet->opcode;
    Dlog("\n%lu: Packet: type: %d", my_addr, opcode);
    switch (opcode) {
        case MANAGER:
            process_mngr_udp(buf);
            break;
        case ROUTER:
            process_rtr_udp(buf);
            break;
        case ADVT:
            process_advt_udp(buf);
            break;       
        default:
            Dlog("\nUnknown type %d", opcode);
    }
}

void
process_input (int file) {

    Dlog("\n fdinfo 0 %d fdinfo 1 %d file %d", fdinfo[0].fd, fdinfo[1].fd, file);
    if (fdinfo[0].fd == file && !tcp_killed) {
       process_input_tcp(file);
    } else {
       process_input_udp(file);
    }
}

/*
 * This function opens a TCP socket on user defined port, spawns two threads, one for getting 
 *  and valioating data from the client and another for interpreting this data and pasting 
 *  the data to a file.
 */ 
int main (int argc, char *argv[])
{
    int sockfd, res;
    int count = 0; //Describing the number of Fds to be polled
    struct addrinfo hints, *servinfo, *p;
    int rv, i;
    struct sockaddr_in sin = {0};
    if (argc != 4) {
    fprintf(stderr,"usage: client hostname tcp_port udp_port\n");
        exit(1);
    }

    /*
     * Setup the TCP port to talk to the simulation manager.
     */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    strncpy(hostname, argv[1], 100);
    Dlog("\n Hostname: %s", hostname);
    if ((rv = getaddrinfo(hostname, argv[2], &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return 1;
    }

    /*
     * Loop through all the results and connect to the first we can
     */
    for(p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype,
            p->ai_protocol)) == -1) {
        perror("client: socket");
        continue;
    }

    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
        close(sockfd);
        perror("client: connect");
        continue;
    }

    break;
    }

    if (p == NULL) {
    fprintf(stderr, "client: failed to connect\n");
    return 2;
    }

    freeaddrinfo(servinfo); // all done with this structure
    /*
     * Setup the array of file-descriptprs with the TCP port, we are interested in
     * polking the input.
     */
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&i, sizeof(i));
    fdinfo[0].fd = sockfd;
    fdinfo[0].events = POLLIN;
    count++;
    /*
     * Setup UDP to rx the packet on the designated port, which comes up 
     * on any IP address in the box.
     */
    udp_port = (short)atoi(argv[3]);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    sin.sin_family = AF_INET;
    sin.sin_port = htons(udp_port);
    sin.sin_addr.s_addr = INADDR_ANY;
    bind(sockfd, (struct sockaddr *) &sin, sizeof(sin));

    fdinfo[1].fd = sockfd;
    fdinfo[1].events = POLLIN;
    count++;  
    transmit_fd(fdinfo[0].fd, "HELO\n", 5);
    
    i = fcntl(fdinfo[0].fd, F_SETFL, O_NONBLOCK);
    i = fcntl(fdinfo[1].fd, F_SETFL, O_NONBLOCK);
    while (1) {
        res = poll(fdinfo, count, 1);
        if (res == -1) {
            Dlog("\n res -1, continue");
            sleep (1);
            continue;
        }
        for (i = 0; i < count && (res > 0); i++) {
            if (fdinfo[i].revents != 0) {
                res--;
                Dlog ("\nFound event with fd:%d", i);
                if (fdinfo[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    if (fdinfo[i].revents & (POLLIN))
                        process_input(fdinfo[i].fd);
                    else {
                        Dlog ("\nError in fd %d", i);
                        count--;
                        if (count <= 0) {
                            Dlog ("\n No fds to poll, exit");
                            exit(1);
                        }
                        if (i == 0) {
                            /* Look at only UDP port instead, to allow routers to exhist 
                             * when manager is gone.
                             */
                            fdinfo[0].fd = fdinfo[1].fd;
                            tcp_killed = 1;
                        }
                    }
                } else if (fdinfo[i].revents & (POLLIN)) {
                    process_input(fdinfo[i].fd);
                }
            }
        }
    }

    Dlog("\nMain Client Thread: Done with execution, Output File: file_output\n");
    pthread_exit(NULL);
    return 0;
}

