#pragma once

#include <ntdef.h>

#define __bitwise__
#define __attribute__(x)

#define u8 unsigned char
#define u16 unsigned short
#define u32 unsigned long
#define u64 ULONGLONG

#define __u8 unsigned char
#define __u16 unsigned short
#define __le16 unsigned short
#define __le32 unsigned long
#define __u64 ULONGLONG

typedef __u16 __bitwise__ __virtio16;
typedef UINT32 __bitwise__ __virtio32;
typedef __u64 __bitwise__ __virtio64;

/* Virtio ring descriptors: 16 bytes.  These can chain together via "next". */
struct vring_desc {
	/* Address (guest-physical). */
	__virtio64 addr;
	/* Length. */
	__virtio32 len;
	/* The flags as indicated above. */
	__virtio16 flags;
	/* We chain unused descriptors via this, too */
	__virtio16 next;
};

struct vring_avail {
	__virtio16 flags;
	__virtio16 idx;
	__virtio16 ring[];
};

/* u32 is used here for ids for padding reasons. */
struct vring_used_elem {
	/* Index of start of used descriptor chain. */
	__virtio32 id;
	/* Total length of the descriptor chain which was used (written to) */
	__virtio32 len;
};

struct vring_used {
	__virtio16 flags;
	__virtio16 idx;
	struct vring_used_elem ring[];
};

/* Alignment requirements for vring elements.
* When using pre-virtio 1.0 layout, these fall out naturally.
*/
#define VRING_AVAIL_ALIGN_SIZE 2
#define VRING_USED_ALIGN_SIZE 4
#define VRING_DESC_ALIGN_SIZE 16

/* The standard layout for the ring is a continuous chunk of memory which looks
* like this.  We assume num is a power of 2.
*
* struct vring
* {
*	// The actual descriptors (16 bytes each)
*	struct vring_desc desc[num];
*
*	// A ring of available descriptor heads with free-running index.
*	__virtio16 avail_flags;
*	__virtio16 avail_idx;
*	__virtio16 available[num];
*	__virtio16 used_event_idx;
*
*	// Padding to the next align boundary.
*	char pad[];
*
*	// A ring of used descriptor heads with free-running index.
*	__virtio16 used_flags;
*	__virtio16 used_idx;
*	struct vring_used_elem used[num];
*	__virtio16 avail_event_idx;
* };
*/
/* We publish the used event index at the end of the available ring, and vice
* versa. They are at the end for backwards compatibility. */

struct vring {
	unsigned int num;

	struct vring_desc *desc;

	struct vring_avail *avail;

	struct vring_used *used;
};

#define vring_used_event(vr) ((vr)->avail->ring[(vr)->num])
#define vring_avail_event(vr) (*(__virtio16 *)&(vr)->used->ring[(vr)->num])

static inline void vring_init(struct vring *vr, unsigned int num, void *p,
	unsigned long align)
{
	vr->num = num;
	vr->desc = (struct vring_desc *)p;
	vr->avail = (struct vring_avail *)((__u8 *)p + num * sizeof(struct vring_desc));
	vr->used = (struct vring_used *)(((ULONG_PTR)&vr->avail->ring[num] + sizeof(__virtio16)
		+ align - 1) & ~((ULONG_PTR)align - 1));
}

static inline unsigned vring_size(unsigned int num, unsigned long align)
{
	return ((sizeof(struct vring_desc) * num + sizeof(__virtio16) * (3 + num)
		+ align - 1) & ~((ULONG_PTR)align - 1))
		+ sizeof(__virtio16) * 3 + sizeof(struct vring_used_elem) * num;
}

/* The following is used with USED_EVENT_IDX and AVAIL_EVENT_IDX */
/* Assuming a given event_idx value from the other side, if
 * we have just incremented index from old to new_idx,
 * should we trigger an event? */
static inline int vring_need_event(__u16 event_idx, __u16 new_idx, __u16 old)
{
	/* Note: Xen has similar logic for notification hold-off
	 * in include/xen/interface/io/ring.h with req_event and req_prod
	 * corresponding to event_idx + 1 and new_idx respectively.
	 * Note also that req_event and req_prod in Xen start at 1,
	 * event indexes in virtio start at 0. */
	return (__u16)(new_idx - event_idx - 1) < (__u16)(new_idx - old);
}

struct ivshm_net_queue {
	struct vring vr;
	UINT32 free_head;
	UINT32 num_free;
	UINT32 num_added;
	UINT16 last_avail_idx;
	UINT16 last_used_idx;

	UINT8 *data;
	UINT8 *end;
	UINT32 size;
	UINT32 head;
	UINT32 tail;
};

#define ALIGN(x,a)              __ALIGN_MASK(x,(a)-1)
#define __ALIGN_MASK(x,mask)    (((x)+(mask))&~(mask))

#define IVSHM_NET_VQ_ALIGN 64

#define IVSHM_NET_FRAME_SIZE(s) ALIGN(18 + (s), 64)

NTSTATUS ivshm_net_calc_qsize(_In_ IVN_ADAPTER *adapter);
void ivshm_net_init_queue(_In_ IVN_ADAPTER *adapter,
	struct ivshm_net_queue *q, UINT8 *mem,
	unsigned int len);
void *ivshm_net_desc_data(_In_ IVN_ADAPTER *adapter,
	struct ivshm_net_queue *q,
	struct vring_desc *desc, u32 *len);