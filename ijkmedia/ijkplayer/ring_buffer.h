

#ifndef MW_RING_BUFFER_H
#define MW_RING_BUFFER_H

#define RB_ERR_OK		0
#define RB_ENOMEM		12
#define RB_EFULL		28

typedef unsigned long long USC_Time_t;

typedef struct {
	char *buffer;
	int length;
	USC_Time_t ts;
} item_data;


typedef struct {
	int r_pos;
	int w_pos;

	int nitem;
	item_data *item_array;
}pcm_ring_buffer;


#define ring_buffer_empty(B) ((B)->r_pos == (B)->w_pos)
#define ring_buffer_full(B) ( ((B)->w_pos+1)%(B)->nitem == (B)->r_pos)


int ring_buffer_init(pcm_ring_buffer *prb, int nitem);
void ring_buffer_deinit(pcm_ring_buffer *prb);

int ring_buffer_push(pcm_ring_buffer *prb, char *buffer, int length, USC_Time_t ts);

static inline item_data * ring_buffer_peek(pcm_ring_buffer *prb)
{
	if (ring_buffer_empty(prb))
	{
		return NULL;
	}

	return (&prb->item_array[prb->r_pos]);
}

static inline void ring_buffer_pop(pcm_ring_buffer *prb)
{
	prb->r_pos = (prb->r_pos+1)%prb->nitem;
}

static inline void ring_buffer_reset(pcm_ring_buffer *prb)
{
	prb->r_pos = 0;
	prb->w_pos = 0;
}

static inline int ring_buffer_available(pcm_ring_buffer *prb)
{
	if (prb->w_pos>=prb->r_pos)
	{
		return prb->w_pos - prb->r_pos;
	}

	return prb->nitem + prb->w_pos - prb->r_pos;
}

#endif
