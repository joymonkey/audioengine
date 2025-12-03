//#include "serial_queue.h"
#include "config.h"

// ===================================
// Global Queue Instance
// ===================================
SerialQueue serial2Queue;

// ===================================
// Initialize Queue
// ===================================
void initSerial2Queue() {
    serial2Queue.readPos = 0;
    serial2Queue.writePos = 0;
    serial2Queue.messagesSent = 0;
    serial2Queue.messagesDropped = 0;
}

// ===================================
// Queue a Message for Serial2
// ===================================
bool queueSerial2Message(const char* msg) {
    if (!msg) return false;
    
    int nextWrite = (serial2Queue.writePos + 1) % SERIAL2_QUEUE_SIZE;
    
    // Check if queue is full
    if (nextWrite == serial2Queue.readPos) {
        // Queue full - drop oldest message
        serial2Queue.readPos = (serial2Queue.readPos + 1) % SERIAL2_QUEUE_SIZE;
        serial2Queue.messagesDropped++;
        
        #ifdef DEBUG
        Serial.println("WARNING: Serial2 queue full, dropped oldest message");
        #endif
    }
    
    // Copy message to queue
    SerialMessage* msgSlot = &serial2Queue.messages[serial2Queue.writePos];
    strncpy(msgSlot->buffer, msg, SERIAL2_MSG_MAX_LENGTH - 1);
    msgSlot->buffer[SERIAL2_MSG_MAX_LENGTH - 1] = '\0'; // Ensure null termination
    msgSlot->length = strlen(msgSlot->buffer);
    
    serial2Queue.writePos = nextWrite;
    
    return true;
}

// ===================================
// Check if CPU is Busy
// ===================================
bool isCpuBusy() {
    // Check if any MP3 stream has a buffer running low
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].active && streams[i].type == STREAM_TYPE_MP3_SD) {
            int available = streams[i].ringBuffer->availableForRead();
            // If buffer is less than 25% full, CPU should prioritize refilling
            if (available < (STREAM_BUFFER_SIZE / 4)) {
                return true; // Buffer running low, CPU is busy
            }
        }
    }
    return false; // All buffers healthy, can send messages
}

// ===================================
// Try to Send Queued Messages
// ===================================
void trySendQueuedMessages(int maxMessages) {
    // Don't send if CPU is busy with MP3 decoding
    if (isCpuBusy()) {
        return;
    }
    
    int sent = 0;
    while (sent < maxMessages && serial2Queue.readPos != serial2Queue.writePos) {
        // Get message
        SerialMessage* msg = &serial2Queue.messages[serial2Queue.readPos];
        
        // Send to Serial2
        Serial2.println(msg->buffer);
        
        // Move read pointer
        serial2Queue.readPos = (serial2Queue.readPos + 1) % SERIAL2_QUEUE_SIZE;
        serial2Queue.messagesSent++;
        sent++;
    }
}

// ===================================
// Get Queued Message Count
// ===================================
int getQueuedMessageCount() {
    return (serial2Queue.writePos - serial2Queue.readPos + SERIAL2_QUEUE_SIZE) % SERIAL2_QUEUE_SIZE;
}
