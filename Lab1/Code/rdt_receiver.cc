/*
 * FILE: rdt_receiver.cc
 * DESCRIPTION: Reliable data transfer receiver.
 * NOTE: This implementation assumes there is no packet loss, corruption, or 
 *       reordering.  You will need to enhance it to deal with all these 
 *       situations.  In this implementation, the packet format is laid out as 
 *       the following:
 *       
 *       |<-  1 byte  ->|<-             the rest            ->|
 *       | payload size |<-             payload             ->|
 *
 *       The first byte of each packet indicates the size of the payload
 *       (excluding this single-byte header)
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

#include "rdt_struct.h"
#include "rdt_receiver.h"

std::mutex rdt_receiver_mutex;


/*
* NOTE: My Own Realization of ACK Structure is:
*       First 4 Byte For Packet Sequence ID Number;
*       Then 4 Byte For hash(ID sequence + ifACK) avoid content being changed during transfer 
*       repeat possibility is 1/2^32
*       |<-  4 byte  ->|<-    4 byte  ->|
*       | ID sequence  | HashedCheckNum |
*/

// var for ACK packet info
const int ACK_ID_Sequence_Length = 4;
const int ACK_HashedCheckNum_Lenght = 4;

const int ACK_ID_Sequence_Offset = 0;
const int ACK_HashedCheckNum_Offset = ACK_ID_Sequence_Length;

const bool ACK_Value = true;

// num info about packet header
const int Packet_ID_Sequence_Length = 4;
const int Packet_HashedCheckNum_Length = 4;
const int Packet_PayloadSize_Length = 1;
const int Packet_Payload_Length = RDT_PKTSIZE - Packet_ID_Sequence_Length - Packet_HashedCheckNum_Length - Packet_PayloadSize_Length;

// calculate info offset according num info about packet header
const int Packet_ID_Sequence_Offset = 0;
const int Packet_HashedCheckNum_Offset = Packet_ID_Sequence_Length;
const int Packet_PayloadSize_Offset = Packet_ID_Sequence_Length + Packet_HashedCheckNum_Length;
const int Packet_Payload_Offset = Packet_ID_Sequence_Length + Packet_HashedCheckNum_Length + Packet_PayloadSize_Length;


// hash func
std::hash<std::string> hash_str_receiver;

// 已经收到的最大的包
// 按照GBN的协议，假如收到1、2、4，在收到4的时候发的ack是2，那4我们用不用缓存呢？答案暂时不
unsigned int Already_Received_Max_ID_Sequence = 0;


void GBN_Ack(unsigned int seqID){
    // 提示：记得回收内存！
    // 超级大坑！在哪回收内存！不是在接收ack的sender的函数回收内存，是在这里直接回收！因为Receiver_ToLowerLayer函数会做一个深度拷贝！
    
    struct packet* ack_resp = (struct packet*) malloc(sizeof(struct packet));
    if(ack_resp != NULL){
        unsigned int HashedStr = hash_str_receiver(std::to_string(seqID));
        memcpy(ack_resp->data + ACK_ID_Sequence_Offset,&seqID, ACK_ID_Sequence_Length);
        memcpy(ack_resp->data + ACK_HashedCheckNum_Offset, &HashedStr, ACK_HashedCheckNum_Lenght);
        Receiver_ToLowerLayer(ack_resp);
        
        free(ack_resp);
    }
}


/* receiver initialization, called once at the very beginning */
void Receiver_Init()
{
    fprintf(stdout, "At %.2fs: receiver initializing ...\n", GetSimulationTime());
}

/* receiver finalization, called once at the very end.
   you may find that you don't need it, in which case you can leave it blank.
   in certain cases, you might want to use this opportunity to release some 
   memory you allocated in Receiver_init(). */
void Receiver_Final()
{
    fprintf(stdout, "At %.2fs: receiver finalizing ...\n", GetSimulationTime());
}


void Receiver_FromLowerLayer_Original(struct packet *pkt){
    /* 1-byte header indicating the size of the payload */
    int header_size = 1;

    /* construct a message and deliver to the upper layer */
    struct message *msg = (struct message*) malloc(sizeof(struct message));
    ASSERT(msg!=NULL);

    msg->size = pkt->data[0];

    /* sanity check in case the packet is corrupted */
    if (msg->size<0) msg->size=0;
    if (msg->size>RDT_PKTSIZE-header_size) msg->size=RDT_PKTSIZE-header_size;

    msg->data = (char*) malloc(msg->size);
    ASSERT(msg->data!=NULL);
    memcpy(msg->data, pkt->data+header_size, msg->size);
    Receiver_ToUpperLayer(msg);

    /* don't forget to free the space */
    if (msg->data!=NULL) free(msg->data);
    if (msg!=NULL) free(msg);
}

// 收到之后要开始拆包 校验
void Receiver_FromLowerLayer_GBN(struct packet *pkt){
    ASSERT(pkt != NULL)
    rdt_receiver_mutex.lock();

    // printf("[Receiver_FromLowerLayer_GBN] start \n");
    // 获取包里面的相关值
    unsigned int Part_SeqID = 0;
    memcpy(&Part_SeqID, pkt -> data + Packet_ID_Sequence_Offset, Packet_ID_Sequence_Length);
    
    // 把Hash Value拷贝过去
    unsigned int Part_Hashed = 0;
    memcpy(&Part_Hashed, pkt -> data + Packet_HashedCheckNum_Offset, Packet_HashedCheckNum_Length);
    
    // 把Payload Length拷贝过去
    unsigned char Part_PayloadLength = 0;
    memcpy(&Part_PayloadLength, pkt -> data + Packet_PayloadSize_Offset, Packet_PayloadSize_Length);

    
    
    if((unsigned int)Part_PayloadLength <= Packet_Payload_Length){
        std::string dataStr(pkt -> data + Packet_Payload_Offset, pkt -> data + Packet_Payload_Offset + Part_PayloadLength);
        //printf("%u !\n", Part_PayloadLength);
        // 开始校验是否信息被篡改过！
        unsigned int hash_cal = hash_str_receiver(std::to_string(Part_SeqID) + std::to_string(Part_PayloadLength) + dataStr);
        // printf("[Receiver_FromLowerLayer_GBN] point1 \n");
        // 消息没有被篡改 
        if(hash_cal == Part_Hashed){
            
            // 乱序了！这时候处理方法，send 之前已经顺序收到的最大的ACK，
            // 举例：依次收到1、2、3、6，处理6的收到的时候，发的ack是ack3！这个包裹也不往上传输，等下一回重发
            if(Part_SeqID != Already_Received_Max_ID_Sequence + 1){
                GBN_Ack(Already_Received_Max_ID_Sequence);
            }
            // 且是下一个要接受的包，没有乱序
            else{
                Already_Received_Max_ID_Sequence = Part_SeqID;
                // 定义一个rest msg
                struct message *rest_msg = (struct message*) malloc(sizeof(struct message));
                ASSERT(rest_msg != NULL)
                
                // 把payload 拷贝过去
                
                rest_msg -> size = (int)Part_PayloadLength;
                rest_msg -> data = (char*)malloc(rest_msg ->size);

                ASSERT(rest_msg -> data != NULL);
                memcpy(rest_msg -> data, pkt -> data + Packet_Payload_Offset,  Part_PayloadLength);

                Receiver_ToUpperLayer(rest_msg);
                GBN_Ack(Part_SeqID);

                
                if(rest_msg ->data != NULL) free(rest_msg ->data);
                if(rest_msg != NULL) free(rest_msg);
                
            }
        }
    }

    // memcpy(pkt -> data + Packet_Payload_Offset, msg -> data + cursor, Part_PayloadLength);

    // printf("[Receiver_FromLowerLayer_GBN] end \n");
    rdt_receiver_mutex.unlock();
}


/* event handler, called when a packet is passed from the lower layer at the 
   receiver */
void Receiver_FromLowerLayer(struct packet *pkt)
{
    Receiver_FromLowerLayer_GBN(pkt);
}


