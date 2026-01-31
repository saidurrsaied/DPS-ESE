//File: event.c

#define _POSIX_C_SOURCE 200809L

#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "truckplatoon.h"
#include "event.h"




/*Queue And Scheduling Related Functions */

//FUNC: For initializing event queue 
void event_queue_init(EventQueue* queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutexattr_init(&queue->attr_eventQueue);
    pthread_mutexattr_setprotocol(&queue->attr_eventQueue, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&queue->mutex_eventQueue, &queue->attr_eventQueue);
    pthread_cond_init(&queue->cond_eventQueue, NULL);   
}


//FUNC: PUSH events

void push_event(EventQueue* queue, Event* event) {
    pthread_mutex_lock(&queue->mutex_eventQueue);

    EventRing* evnt_ring = &queue->eventRings[event->type];
    int next_tail = (evnt_ring->tail + 1) % MAX_EVENTS;

    if (next_tail == evnt_ring->head) {
        // queue full
        fprintf(stderr, "Event queue full for type %d\n", event->type);
        pthread_mutex_unlock(&queue->mutex_eventQueue);
        return;
    }

    evnt_ring->queue[evnt_ring->tail] = *event;
    evnt_ring->tail = next_tail;

    pthread_cond_signal(&queue->cond_eventQueue);
    pthread_mutex_unlock(&queue->mutex_eventQueue);
}


//FUNC: Pull Events
Event pop_event(EventQueue* queue) {
    pthread_mutex_lock(&queue->mutex_eventQueue);

    while (1) {
        for (int p = 0; p < NUM_PRIORITIES; p++) {
            EventRing* evnt_ring = &queue->eventRings[p];
            if (evnt_ring->head != evnt_ring->tail) {
                Event evnt = evnt_ring->queue[evnt_ring->head];
                evnt_ring->head = (evnt_ring->head + 1) % MAX_EVENTS;
                pthread_mutex_unlock(&queue->mutex_eventQueue);
                return evnt;
            }
        }

        // sleep if no events
        pthread_cond_wait(&queue->cond_eventQueue, &queue->mutex_eventQueue);
    }
}