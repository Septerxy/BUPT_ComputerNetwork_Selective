#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "datalink.h"

typedef enum
{
	false, true
}bool;


/* 定义包类型 */
typedef struct
{
	unsigned char info[PKT_LEN];
}packet;

/* 定义帧类型 */
typedef struct FRAME
{
	unsigned char kind; /* 帧的种类 */
	unsigned char ack; /* ACK号 */
	unsigned char seq; /* 序列号 */
	packet data; /* 需要被转发的数据 */
	unsigned int  padding;
}FRAME;

/* 发送窗口上限 */
static unsigned char next_frame_to_send;
/* 发送窗口下限 */
static unsigned char ack_expected;
/* 接收窗口上限 */
static unsigned char too_far;
/* 接收窗口下限 */
static unsigned char frame_expected;
/* 计数，发了多少个 */
static unsigned char nbuffered;

/* 数据帧超时机制 */
#define DATA_TIMER  5000 
/* ACK超时机制 */
#define ACK_TIMER 280
/* 帧的最大长度 */
#define MAX_SEQ 31
/* 窗口大小 */
#define NR_BUFS ((MAX_SEQ + 1) / 2) 
/* 实现k的按模 +1 */
#define inc(k) if(k < MAX_SEQ)k++; else k=0 



/* phl_ready = 0 意味着物理层还没有准备好 */
static int phl_ready = 0;
/* no_nak = true 意味着还没有发送过一个NAK */
bool no_nak = true;




/* 判断数据帧是否位于接受窗口内，来判断是否暂存 */
static int between(unsigned char a, unsigned char b, unsigned char c)
{
	return ((a <= b && b < c) || (c < a&& a <= b) || (b < c&& c < a));
}

/* 向网络层上交数据帧 */
static void put_frame(unsigned char* frame, int len)
{
	*(unsigned int*)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

/* 发出帧，此处按帧的类型用if语句进行不同的操作，所以我将三个函数合在一起 */
static void send_data(unsigned char fk, unsigned char frame_nr, unsigned char frame_expected, packet buffer[])
{
	/* 创建一个空白新帧备用 */
	FRAME s;
	s.kind = fk;
	/* s.seq 只对数据帧有意义 */
	s.seq = frame_nr;
	/* 累积确认，表示最后一个正确收到的 */
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);

	/* 如果是数据帧 */
	if (fk == FRAME_DATA)
	{
		/* 将数据帧复制到 s 中去 */
		memcpy(s.data.info, buffer[frame_nr % NR_BUFS].info, PKT_LEN);
		/* 打印信息 */
		dbg_frame("Send DATA %d %d,ID %d\n", s.seq, s.ack, *(short*)&(s.data).info);
		/* 将数据帧下发给物理层 */
		put_frame((unsigned char*)&s, 3 + PKT_LEN);
		/* 发送数据帧时，启动数据帧计时器 */
		start_timer(frame_nr % NR_BUFS, DATA_TIMER);
	}

	/* 如果是NAK帧 */
	if (fk == FRAME_NAK)
	{
		/* 已经发了一个NAK，no_nak置false */
		no_nak = false;
		/* 将NAK帧下发给物理层 */
		put_frame((unsigned char*)&s, 2);
	}

	/* 如果是ACK帧 */
	if (fk == FRAME_ACK)
	{
		/* 打印 */
		dbg_frame("Send ACK %d\n", s.ack);
		/* 将ACK帧下发给物理层 */
		put_frame((unsigned char*)&s, 2);
	}

	/* 发送帧时停止 */
	stop_ack_timer();
}



int main(int argc, char** argv)
{
	/*
		事件，在protocol.h中已完成定义
		#define NETWORK_LAYER_READY  0
		#define PHYSICAL_LAYER_READY 1
		#define FRAME_RECEIVED       2
		#define DATA_TIMEOUT         3
		#define ACK_TIMEOUT          4
	*/
	int event, arg;
	FRAME f;
	int len = 0;
	int i;


	/* 发送窗口的“椅子” */
	packet out_buf[NR_BUFS];
	/* 接受窗口的“椅子” */
	packet in_buf[NR_BUFS];
	/* 收到的数据帧该坐的“椅子”当前是否空闲 */
	bool arrived[NR_BUFS];

	/* 初始化 */
	/* 接收端初始化时，enable_network_layer() 被抑制 */
	enable_network_layer();
	ack_expected = 0;
	next_frame_to_send = 0;
	frame_expected = 0;
	too_far = NR_BUFS;
	nbuffered = 0;

	for (i = 0; i < NR_BUFS; i++)
	{
		arrived[i] = false;
	}

	/* Create Communication Sockets */
	protocol_init(argc, argv);
	/* 打印作者信息、时间 */
	lprintf("Designed by 2018210074 - Xiong Yu, build: " __DATE__"  "__TIME__"\n");



	for (; ; )
	{
		event = wait_for_event(&arg);

		switch (event)
		{
			/* 对于发送端，网络层有信息要发送 */
		case NETWORK_LAYER_READY:
			/* 为发新帧做准备 */
			nbuffered++;
			/* 取数据 */
			get_packet(out_buf[next_frame_to_send % NR_BUFS].info);
			/* 发送DATA型数据帧 */
			send_data(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);
			/* 发送窗口上限 +1 */
			inc(next_frame_to_send);
			break;

			/* 物理层有信息来到 */
		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

			/* 接收到帧 */
		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&f, sizeof f);
			/* 检查帧是否正常 */
			if (len < 5 || crc32((unsigned char*)&f, len) != 0)
			{
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				/* 如果之前没发过NAK，那么就要发一个NAK */
				if (no_nak)
				{
					send_data(FRAME_NAK, 0, frame_expected, out_buf);
				}
				break;
			}
			/*if (f.kind == FRAME_ACK)
				dbg_frame("Recv ACK  %d\n", f.ack);*/

				/*
				   如果是数据帧，则检查帧序号，落在接收窗口内接收，否则丢弃
				   如果不等于接收窗口下限，且没发过NAK，还得发一个NAK
				*/
			if (f.kind == FRAME_DATA)
			{

				if ((f.seq != frame_expected) && no_nak)
					send_data(FRAME_NAK, 0, frame_expected, out_buf);
				else
					/* 无法捎带，则单发ACK */
					start_ack_timer(ACK_TIMER);

				if (between(frame_expected, f.seq, too_far) && (arrived[f.seq % NR_BUFS] == false))
				{
					dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)&(f.data).info);
					/* 这个“椅子”有人坐了，不再空闲 */
					arrived[f.seq % NR_BUFS] = true;
					/* 信息写入该椅子 */
					in_buf[f.seq % NR_BUFS] = f.data;
					/* 确保按次序依次上交，当frame_expected这把椅子为true时才上交 */
					while (arrived[frame_expected % NR_BUFS])
					{
						put_packet(in_buf[frame_expected % NR_BUFS].info, len - 7);
						no_nak = true;
						/* 让接收端的该椅子仍然可以接受数据 */
						arrived[frame_expected % NR_BUFS] = false;
						/* 接收窗口上限、下限 +1 */
						inc(too_far);
						inc(frame_expected);

						start_ack_timer(ACK_TIMER);
					}
				}
			}

			/* 如果是NAK帧，则选择重传 */
			if ((f.kind == FRAME_NAK) && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
			{
				dbg_frame("Recv NAK DATA %d\n", f.ack);
				send_data(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
			}


			/* 检查确认序号，落在发送窗口内则移动发送窗口，否则不做处理 */
			while (between(ack_expected, f.ack, next_frame_to_send))
			{
				nbuffered--;
				/* 收到正确确认，停止计时器 */
				stop_timer(ack_expected % NR_BUFS);
				inc(ack_expected);
			}
			break;

			/* ACK超时，就单发一个ACK */
		case ACK_TIMEOUT:
			dbg_event("******** DATA %d timeout *********\n", arg);
			send_data(FRAME_ACK, 0, frame_expected, out_buf);
			break;

			/* 超时 选择重传*/
		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			if (!between(ack_expected, arg, next_frame_to_send))
				arg = arg + NR_BUFS;
			send_data(FRAME_DATA, arg, frame_expected, out_buf);
			break;
		}

		/* 内存没满，继续进行 */
		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		/* 内存满了，停止 */
		else
			disable_network_layer();
	}
}









