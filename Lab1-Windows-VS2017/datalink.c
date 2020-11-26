#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "datalink.h"

typedef enum
{
	false, true
}bool;


/* ��������� */
typedef struct
{
	unsigned char info[PKT_LEN];
}packet;

/* ����֡���� */
typedef struct FRAME
{
	unsigned char kind; /* ֡������ */
	unsigned char ack; /* ACK�� */
	unsigned char seq; /* ���к� */
	packet data; /* ��Ҫ��ת�������� */
	unsigned int  padding;
}FRAME;

/* ���ʹ������� */
static unsigned char next_frame_to_send;
/* ���ʹ������� */
static unsigned char ack_expected;
/* ���մ������� */
static unsigned char too_far;
/* ���մ������� */
static unsigned char frame_expected;
/* ���������˶��ٸ� */
static unsigned char nbuffered;

/* ����֡��ʱ���� */
#define DATA_TIMER  5000 
/* ACK��ʱ���� */
#define ACK_TIMER 280
/* ֡����󳤶� */
#define MAX_SEQ 31
/* ���ڴ�С */
#define NR_BUFS ((MAX_SEQ + 1) / 2) 
/* ʵ��k�İ�ģ +1 */
#define inc(k) if(k < MAX_SEQ)k++; else k=0 



/* phl_ready = 0 ��ζ������㻹û��׼���� */
static int phl_ready = 0;
/* no_nak = true ��ζ�Ż�û�з��͹�һ��NAK */
bool no_nak = true;




/* �ж�����֡�Ƿ�λ�ڽ��ܴ����ڣ����ж��Ƿ��ݴ� */
static int between(unsigned char a, unsigned char b, unsigned char c)
{
	return ((a <= b && b < c) || (c < a&& a <= b) || (b < c&& c < a));
}

/* ��������Ͻ�����֡ */
static void put_frame(unsigned char* frame, int len)
{
	*(unsigned int*)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

/* ����֡���˴���֡��������if�����в�ͬ�Ĳ����������ҽ�������������һ�� */
static void send_data(unsigned char fk, unsigned char frame_nr, unsigned char frame_expected, packet buffer[])
{
	/* ����һ���հ���֡���� */
	FRAME s;
	s.kind = fk;
	/* s.seq ֻ������֡������ */
	s.seq = frame_nr;
	/* �ۻ�ȷ�ϣ���ʾ���һ����ȷ�յ��� */
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);

	/* ���������֡ */
	if (fk == FRAME_DATA)
	{
		/* ������֡���Ƶ� s ��ȥ */
		memcpy(s.data.info, buffer[frame_nr % NR_BUFS].info, PKT_LEN);
		/* ��ӡ��Ϣ */
		dbg_frame("Send DATA %d %d,ID %d\n", s.seq, s.ack, *(short*)&(s.data).info);
		/* ������֡�·�������� */
		put_frame((unsigned char*)&s, 3 + PKT_LEN);
		/* ��������֡ʱ����������֡��ʱ�� */
		start_timer(frame_nr % NR_BUFS, DATA_TIMER);
	}

	/* �����NAK֡ */
	if (fk == FRAME_NAK)
	{
		/* �Ѿ�����һ��NAK��no_nak��false */
		no_nak = false;
		/* ��NAK֡�·�������� */
		put_frame((unsigned char*)&s, 2);
	}

	/* �����ACK֡ */
	if (fk == FRAME_ACK)
	{
		/* ��ӡ */
		dbg_frame("Send ACK %d\n", s.ack);
		/* ��ACK֡�·�������� */
		put_frame((unsigned char*)&s, 2);
	}

	/* ����֡ʱֹͣ */
	stop_ack_timer();
}



int main(int argc, char** argv)
{
	/*
		�¼�����protocol.h������ɶ���
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


	/* ���ʹ��ڵġ����ӡ� */
	packet out_buf[NR_BUFS];
	/* ���ܴ��ڵġ����ӡ� */
	packet in_buf[NR_BUFS];
	/* �յ�������֡�����ġ����ӡ���ǰ�Ƿ���� */
	bool arrived[NR_BUFS];

	/* ��ʼ�� */
	/* ���ն˳�ʼ��ʱ��enable_network_layer() ������ */
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
	/* ��ӡ������Ϣ��ʱ�� */
	lprintf("Designed by 2018210074 - Xiong Yu, build: " __DATE__"  "__TIME__"\n");



	for (; ; )
	{
		event = wait_for_event(&arg);

		switch (event)
		{
			/* ���ڷ��Ͷˣ����������ϢҪ���� */
		case NETWORK_LAYER_READY:
			/* Ϊ����֡��׼�� */
			nbuffered++;
			/* ȡ���� */
			get_packet(out_buf[next_frame_to_send % NR_BUFS].info);
			/* ����DATA������֡ */
			send_data(FRAME_DATA, next_frame_to_send, frame_expected, out_buf);
			/* ���ʹ������� +1 */
			inc(next_frame_to_send);
			break;

			/* ���������Ϣ���� */
		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

			/* ���յ�֡ */
		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&f, sizeof f);
			/* ���֡�Ƿ����� */
			if (len < 5 || crc32((unsigned char*)&f, len) != 0)
			{
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				/* ���֮ǰû����NAK����ô��Ҫ��һ��NAK */
				if (no_nak)
				{
					send_data(FRAME_NAK, 0, frame_expected, out_buf);
				}
				break;
			}
			/*if (f.kind == FRAME_ACK)
				dbg_frame("Recv ACK  %d\n", f.ack);*/

				/*
				   ���������֡������֡��ţ����ڽ��մ����ڽ��գ�������
				   ��������ڽ��մ������ޣ���û����NAK�����÷�һ��NAK
				*/
			if (f.kind == FRAME_DATA)
			{

				if ((f.seq != frame_expected) && no_nak)
					send_data(FRAME_NAK, 0, frame_expected, out_buf);
				else
					/* �޷��Ӵ����򵥷�ACK */
					start_ack_timer(ACK_TIMER);

				if (between(frame_expected, f.seq, too_far) && (arrived[f.seq % NR_BUFS] == false))
				{
					dbg_frame("Recv DATA %d %d, ID %d\n", f.seq, f.ack, *(short*)&(f.data).info);
					/* ��������ӡ��������ˣ����ٿ��� */
					arrived[f.seq % NR_BUFS] = true;
					/* ��Ϣд������� */
					in_buf[f.seq % NR_BUFS] = f.data;
					/* ȷ�������������Ͻ�����frame_expected�������Ϊtrueʱ���Ͻ� */
					while (arrived[frame_expected % NR_BUFS])
					{
						put_packet(in_buf[frame_expected % NR_BUFS].info, len - 7);
						no_nak = true;
						/* �ý��ն˵ĸ�������Ȼ���Խ������� */
						arrived[frame_expected % NR_BUFS] = false;
						/* ���մ������ޡ����� +1 */
						inc(too_far);
						inc(frame_expected);

						start_ack_timer(ACK_TIMER);
					}
				}
			}

			/* �����NAK֡����ѡ���ش� */
			if ((f.kind == FRAME_NAK) && between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send))
			{
				dbg_frame("Recv NAK DATA %d\n", f.ack);
				send_data(FRAME_DATA, (f.ack + 1) % (MAX_SEQ + 1), frame_expected, out_buf);
			}


			/* ���ȷ����ţ����ڷ��ʹ��������ƶ����ʹ��ڣ����������� */
			while (between(ack_expected, f.ack, next_frame_to_send))
			{
				nbuffered--;
				/* �յ���ȷȷ�ϣ�ֹͣ��ʱ�� */
				stop_timer(ack_expected % NR_BUFS);
				inc(ack_expected);
			}
			break;

			/* ACK��ʱ���͵���һ��ACK */
		case ACK_TIMEOUT:
			dbg_event("******** DATA %d timeout *********\n", arg);
			send_data(FRAME_ACK, 0, frame_expected, out_buf);
			break;

			/* ��ʱ ѡ���ش�*/
		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			if (!between(ack_expected, arg, next_frame_to_send))
				arg = arg + NR_BUFS;
			send_data(FRAME_DATA, arg, frame_expected, out_buf);
			break;
		}

		/* �ڴ�û������������ */
		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		/* �ڴ����ˣ�ֹͣ */
		else
			disable_network_layer();
	}
}









