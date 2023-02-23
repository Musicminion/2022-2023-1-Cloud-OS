/*
 * FILE: rdt_sender.cc
 * DESCRIPTION: Reliable data transfer sender.
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

// Plan
// 1. 先写Sender的 Sender_FromUpperLayer_GBN，设计消息的包的格式，发送机制，
// 2. 再写Receiver的 Receiver_FromLowerLayer_GBN，设计收到包裹的处理方式，发ACK
// 3. 再写Sender的 Sender_FromLowerLayer_GBN，设计处理ACK的方式，
// 4. 再写Sender的 Sender_Timeout，处理超时重发所有包
// 5. 测试找bug！
// 6. 同样的方法 写SR选择回传


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <deque>
#include <map>
#include <string>


#include "rdt_struct.h"
#include "rdt_sender.h"


/* some self defined var */
// mutex for avoid conflict
std::mutex rdt_sender_mutex;
// buffer for message send,
std::deque<packet> rdt_sender_buffer;
// record ack received, and for selective reTransfer
std::map<unsigned int, bool> rdt_sender_ackRecord;


/*
* NOTE: My Own Realization of Packet Structure is:
*       First 4 Byte For Packet Sequence ID Number;
*       Then  4 Byte For hash(ID sequence + payload size + payload) avoid content being changed during transfer 
*       repeat possibility is 1/2^32
*       Then  1 Byte For payload size, as original realization.
*       |<-  4 byte  ->|<-    4 byte  ->|<-  1 byte  ->|<-             the rest            ->|
*       | ID sequence  | HashedCheckNum | payload size |<-             payload             ->|
*
*/

// num info about packet header
const int Packet_ID_Sequence_Length = 4;
const int Packet_HashedCheckNum_Length = 4;
const int Packet_PayloadSize_Length = 1;
const int Packet_Header_Length = Packet_ID_Sequence_Length + Packet_HashedCheckNum_Length + Packet_PayloadSize_Length;
const int Packet_Payload_Length = RDT_PKTSIZE - Packet_ID_Sequence_Length - Packet_HashedCheckNum_Length - Packet_PayloadSize_Length;

// calculate info offset according num info about packet header
const int Packet_ID_Sequence_Offset = 0;
const int Packet_HashedCheckNum_Offset = Packet_ID_Sequence_Length;
const int Packet_PayloadSize_Offset = Packet_ID_Sequence_Length + Packet_HashedCheckNum_Length;
const int Packet_Payload_Offset = Packet_ID_Sequence_Length + Packet_HashedCheckNum_Length + Packet_PayloadSize_Length;


// var for ACK packet info
const int ACK_ID_Sequence_Length = 4;
const int ACK_HashedCheckNum_Lenght = 4;

const int ACK_ID_Sequence_Offset = 0;
const int ACK_HashedCheckNum_Offset = ACK_ID_Sequence_Length;




// for set timer, according to lab requirement, 0.3 is suggested
const double TimeOutValue = 0.3;

// window info
const int Moving_Window_Size = 10;
int Moving_Window_Left_Index = 0;
int Moving_Window_Right_Index = 0;

// 引入已经发的包的最大的seqID，不管这个包发没有发成功
// 这个变量是为了方便滑动窗口，当滑动窗口滑动之后，如果起点是一个全新的包，从来没有发送过，那说明窗口里面的所有内容需要重发
// 如果没有这个变量，每次滑动窗口之后，都需把窗口里面信息全发一遍，这是没有必要浪费时间的！
// packet already sent max id
int Already_Send_Max_ID_Sequence = 0;


// hash func
std::hash<std::string> hash_str_sender;


/* sender initialization, called once at the very beginning */
void Sender_Init()
{
    fprintf(stdout, "At %.2fs: sender initializing ...\n", GetSimulationTime());

    rdt_sender_buffer.clear();
    // init move window left point to 0, right to window size - 1
    Moving_Window_Left_Index = 0;
    Moving_Window_Right_Index = Moving_Window_Size - 1;
}

/* sender finalization, called once at the very end.
   you may find that you don't need it, in which case you can leave it blank.
   in certain cases, you might want to take this opportunity to release some 
   memory you allocated in Sender_init(). */
void Sender_Final()
{
    fprintf(stdout, "At %.2fs: sender finalizing ...\n", GetSimulationTime());
    rdt_sender_buffer.clear();
    Moving_Window_Left_Index = 0;
    Moving_Window_Right_Index = Moving_Window_Size - 1;
}

// 这个是原版提供，为了展示对比效果，我单独把原始的代码部分抽象提出来了
// 一些英文的注释还是保留下来啦！
void Sender_FromUpperLayer_Original(struct message *msg){
    // 他这个的实现原理就是：把发送出去的数据包分为两个部分，head部分1byte，剩下的127Byte
    /* 1-byte header indicating the size of the payload */
    int header_size = 1;

    /* maximum payload size */
    int maxpayload_size = RDT_PKTSIZE - header_size;

    /* split the message if it is too big */

    /* reuse the same packet data structure */
    packet pkt;

    /* the cursor always points to the first unsent byte in the message */
    int cursor = 0;

    //  如果发现数据包太大了，那我们就分批次发送，每次发送一部分，用一个while循环来解决
    //  cursor理解为一个指针，指向每次要发送消息的头部，然后发送完之后就移动到下次要发送的开头
    while (msg->size - cursor > maxpayload_size) {
        /* fill in the packet */
        pkt.data[0] = maxpayload_size;
        memcpy(pkt.data + header_size, msg->data+cursor, maxpayload_size);

        /* send it out through the lower layer */
        Sender_ToLowerLayer(&pkt);

        /* move the cursor */
        cursor += maxpayload_size;
    }

    // while执行完之后，还可能有最后一个包没有发送
    /* send out the last packet */
    if (msg->size > cursor) {
        /* fill in the packet */
        pkt.data[0] = msg->size-cursor;
        memcpy(pkt.data+header_size, msg->data+cursor, pkt.data[0]);

        /* send it out through the lower layer */
        Sender_ToLowerLayer(&pkt);
    }

    // 显然这种方式发包，可能会有丢包、传输信息被修改的风险，或者乱序的各种问题，所以不合适不可靠
}


// 采用GBN协议，进行数据传输
void Sender_FromUpperLayer_GBN(struct message *msg){
    rdt_sender_mutex.lock();
    
    packet pkt;
    for(int cursor = 0; cursor < msg ->size; cursor += Packet_Payload_Length){
        /* take msg into many Parts */
        /* Calculate currently Packet Seq ID and this Part Payload Length */
        /* Start from 1, to ++.. */
        
        unsigned int Part_SeqID = rdt_sender_buffer.size() + 1;

        unsigned char Part_PayloadLength = std::min(msg -> size - cursor, Packet_Payload_Length);
        /* convert data to string in order to joint */
        std::string dataStr(msg -> data + cursor, msg -> data + cursor +  Part_PayloadLength);
        /* get hashed val */
        unsigned int Part_Hashed = hash_str_sender(std::to_string(Part_SeqID) + std::to_string(Part_PayloadLength) + dataStr);
        
        /* fill in the packet */
        // 把SeqID拷贝过去
        memcpy(pkt.data + Packet_ID_Sequence_Offset , &Part_SeqID, Packet_ID_Sequence_Length);
        // 把Hash Value拷贝过去
        memcpy(pkt.data + Packet_HashedCheckNum_Offset, &Part_Hashed, Packet_HashedCheckNum_Length);
        // 把payload size 拷贝过去
        memcpy(pkt.data + Packet_PayloadSize_Offset, &Part_PayloadLength, Packet_PayloadSize_Length);
        // 把payload 拷贝过去
        memcpy(pkt.data + Packet_Payload_Offset, msg -> data + cursor, Part_PayloadLength);
        
        
        rdt_sender_buffer.push_back(pkt);
    }
    
    // GBN 发前面的N个
    for(int i = Moving_Window_Left_Index; i <= Moving_Window_Right_Index && i <rdt_sender_buffer.size(); i++){
        Sender_ToLowerLayer(&rdt_sender_buffer[i]);
        // buffer 里面的 buffer[i] 对应seqID i+1
        Already_Send_Max_ID_Sequence = std::max(Already_Send_Max_ID_Sequence, (i + 1));
    }
    
    Sender_StartTimer(TimeOutValue);
    rdt_sender_mutex.unlock();
}


void Sender_FromLowerLayer_GBN(struct packet *pkt)
{
    ASSERT(pkt != NULL)

    rdt_sender_mutex.lock();
    // 拷贝
    unsigned int Part_SeqID;
    memcpy(&Part_SeqID, pkt ->data + ACK_ID_Sequence_Offset, ACK_ID_Sequence_Length);
    unsigned int Hashed_Val;
    memcpy(&Hashed_Val, pkt -> data + ACK_HashedCheckNum_Offset, ACK_HashedCheckNum_Lenght);

    unsigned int hash_cal = hash_str_sender(std::to_string(Part_SeqID));

    // 数据包不存在被篡改, 考虑到GBN的累计确认特性，比如收到ACK的顺序是6,3,4，
    // 那么我收到6的时候就可以认为1-6全部已经被收到了
    // 至于3和4，收到的时候不处理或者丢包了，都可以不管
    if(hash_cal == Hashed_Val && Part_SeqID > Moving_Window_Left_Index){
        // 移动窗口！因为编号为 Part_SeqID 已经被收到了，那么就把窗口的起点往后移动

        // 超级BUG！这里我改了半天才发现！Part_SeqID我是从1开始算的，对应的window的0
        Moving_Window_Left_Index = Part_SeqID;
        Moving_Window_Right_Index = Moving_Window_Left_Index + Moving_Window_Size - 1;
        
        if(Moving_Window_Left_Index > Already_Send_Max_ID_Sequence){
            for(int i = Moving_Window_Left_Index; i <= Moving_Window_Right_Index; i++){
                // 变量 i 非法，不访问直接终止循环
                
                if(i >= rdt_sender_buffer.size())
                    break;
                // 如果这个时候 i == Moving_Window_Left_Index， 说明马上要开始发包了
                // 计时器只需要开启一次，因为是要把整个窗口里面的东西重发一遍
                if(i == Moving_Window_Left_Index)
                    Sender_StartTimer(TimeOutValue);

                Sender_ToLowerLayer(&rdt_sender_buffer[i]);
                Already_Send_Max_ID_Sequence = std::max(Already_Send_Max_ID_Sequence, (i + 1));
            }
        }
    }

    // 注意 这里不需要回收内存，因为检查源代码，上层调用这个函数之后，会主动回收内存！
    rdt_sender_mutex.unlock();
}








void Sender_Timeout_GBN()
{
    rdt_sender_mutex.lock();
    
    // 根据GBN规则，只要超时，全部重新发！
    for(int i = Moving_Window_Left_Index; i <= Moving_Window_Right_Index; i++){
        // 变量 i 非法，不访问直接终止循环
        if(i >= rdt_sender_buffer.size())
            break;
        // 特别注意：发起计时器的时候要谨慎！不然会死循环。
        if(i == Moving_Window_Left_Index)
            Sender_StartTimer(TimeOutValue);
        Sender_ToLowerLayer(&rdt_sender_buffer[i]);
        
        Already_Send_Max_ID_Sequence = std::max(Already_Send_Max_ID_Sequence, (i + 1));
    }

    rdt_sender_mutex.unlock();
}


/* 按照作业要求，我在要求完成的函数上面又一次封装了一层，为了方便的展示GBN和SR两个协议的差别 */
/* 带有GBN的函数是GBN相关的协议的处理函数 */

/* event handler, called when a packet is passed from the lower layer at the 
   sender */
void Sender_FromLowerLayer(struct packet *pkt)
{
    Sender_FromLowerLayer_GBN(pkt);
}




/* event handler, called when the timer expires */
void Sender_Timeout()
{
    Sender_Timeout_GBN();
}


/* event handler, called when a message is passed from the upper layer at the 
   sender */

void Sender_FromUpperLayer(struct message *msg)
{
    // 特别注意！这个函数非常坑！所以一定看上层调用这个函数的逻辑，我也是找了好久才发现自己的bug
    // 我最开始以为上层会产生一个非常长的message，然后我自己切分，结果是发现上层会产生非常多的message
    // 上层函数会随机生成message，message的大小，有时候大，有时候小，大的有时候可能2-3个包裹才能装下
    // 如果message太大，需要在这个函数里面自己切分！这个需要自己实现
    // 如果message比较小，那就直接发出去，一个包裹
    // 因为主函数产生的速度非常快，假如第一次产生的message比较小（分成一个packet），第二次产生的大（需要两个packet）
    // 需要正确的给这些packet标号！第一次产生的message比较小作为一个标号为1的packet，第二次的作为标号为2、3的packet！
    Sender_FromUpperLayer_GBN(msg);
}
