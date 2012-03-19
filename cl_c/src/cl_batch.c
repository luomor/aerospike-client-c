/*
 * The batch interface has some cleverness, it makes parallel requests
 * under the covers to different servers
 *
 *
 * Brian Bulkowski, 2011
 * All rights reserved
 */

#include <sys/types.h>
#include <stdio.h>
#include <errno.h> //errno
#include <stdlib.h> //fprintf
#include <unistd.h> // close
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <zlib.h>


#include "citrusleaf/citrusleaf.h"
#include "citrusleaf/cl_cluster.h"
#include "citrusleaf/citrusleaf-internal.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/proto.h"
#include "citrusleaf/cf_socket.h"


//
// Decompresses a compressed CL msg
// The buffer passed in is the space *after* the header, just the compressed data
//
//
// returns -1 if it can't be decompressed for some reason
//


int
batch_decompress(uint8_t *in_buf, size_t in_sz, uint8_t **out_buf, size_t *out_sz) 
{
	z_stream 	strm;

	uint64_t now = cf_getms();
	
	strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    int rv = inflateInit(&strm);
    if (rv != Z_OK)
        return (-1);

    
    // first 8 bytes are the inflated size, allows efficient alloc (round up: the buf likes that)
    size_t  	b_sz_alloc = *(uint64_t *)in_buf;
    uint8_t 	*b = malloc(b_sz_alloc);
    if (0 == b) {
    	fprintf(stderr, "batch_decompress: could not malloc %"PRIu64" bytes\n",b_sz_alloc);
    	inflateEnd(&strm);
    	return(-1);
    }
    
    strm.avail_in = in_sz - 8;
    strm.next_in = in_buf + 8;
    
	strm.avail_out = b_sz_alloc; // round up: seems to like that
	strm.next_out = b;


//	fprintf(stderr, "before deflate: in_buf %p in_sz %"PRIu64" outbuf %p out_sz %"PRIu64"\n",
//		strm.next_in, strm.avail_in, strm.next_out, strm.avail_out);

	rv = inflate(&strm, Z_FINISH);
	
//	fprintf(stderr, "after deflate: rv %d in_buf %p in_sz %"PRIu64" outbuf %p out_sz %"PRIu64" ms %"PRIu64"\n",
//		rv, strm.next_in, strm.avail_in, strm.next_out, strm.avail_out, cf_getms() - now);

	if (rv != Z_STREAM_END) {
		fprintf(stderr, "could not deflate data: zlib error %d (check zlib.h)\n",rv);
		free(b);
		inflateEnd(&strm);
		return(-1);
	}
    	
	inflateEnd(&strm);
    
    *out_buf = b;
    *out_sz = b_sz_alloc - strm.avail_out;
	
	return(0);	
}


static uint8_t *
write_fields_batch_digests(uint8_t *buf, char *ns, int ns_len, cf_digest *digests, cl_cluster_node **nodes, int n_digests, 
	int n_my_digests, cl_cluster_node *my_node )
{
	
	// lay out the fields
	cl_msg_field *mf = (cl_msg_field *) buf;
	cl_msg_field *mf_tmp = mf;
	
	if (ns) {
		mf->type = CL_MSG_FIELD_TYPE_NAMESPACE;
		mf->field_sz = ns_len + 1;
		memcpy(mf->data, ns, ns_len);
		mf_tmp = cl_msg_field_get_next(mf);
		cl_msg_swap_field(mf);
		mf = mf_tmp;
	}

	mf->type = CL_MSG_FIELD_TYPE_DIGEST_RIPE_ARRAY;
	int digest_sz = sizeof(cf_digest) * n_my_digests;
	mf->field_sz = digest_sz + 1;
	uint8_t *b = mf->data;
	for (int i=0;i<n_digests;i++) {
		if (nodes[i] == my_node) { 
			memcpy(b, &digests[i], sizeof(cf_digest));
			b += sizeof(cf_digest);
		}
	}
		
	mf_tmp = cl_msg_field_get_next(mf);
	cl_msg_swap_field(mf);		
	mf = mf_tmp;

	return ( (uint8_t *) mf_tmp );
}


static int
batch_compile(uint info1, uint info2, char *ns, cf_digest *digests, cl_cluster_node **nodes, int n_digests, cl_cluster_node *my_node, int n_my_digests, cl_bin *values, cl_operator operator, cl_operation *operations, int n_values,  
	uint8_t **buf_r, size_t *buf_sz_r, const cl_write_parameters *cl_w_p)
{
	// I hate strlen
	int		ns_len = ns ? strlen(ns) : 0;
	int		i;
	
	// determine the size
	size_t	msg_sz = sizeof(as_msg); // header
	// fields
	if (ns) msg_sz += ns_len + sizeof(cl_msg_field);
	msg_sz += sizeof(cl_msg_field) + 1 + (sizeof(cf_digest) * n_my_digests);
	// ops
	for (i=0;i<n_values;i++) {
		msg_sz += sizeof(cl_msg_op) + strlen(values[i].bin_name);

        if (0 != value_to_op_get_size(&values[i], &msg_sz)) {
            fprintf(stderr,"illegal parameter: bad type %d write op %d\n",values[i].object.type,i);
            return(-1);
        }
	}
	
	// size too small? malloc!
	uint8_t	*buf;
	uint8_t *mbuf = 0;
	if ((*buf_r) && (msg_sz > *buf_sz_r)) {
		mbuf = buf = malloc(msg_sz);
		if (!buf) 			return(-1);
		*buf_r = buf;
	}
	else
		buf = *buf_r;
	
	*buf_sz_r = msg_sz;
	
	// debug - shouldn't be required
	memset(buf, 0, msg_sz);
	
	// lay in some parameters
	uint32_t generation = 0;
	if (cl_w_p) {
		if (cl_w_p->unique) {
			info2 |= CL_MSG_INFO2_WRITE_UNIQUE;
		}
		else if (cl_w_p->use_generation) {
			info2 |= CL_MSG_INFO2_GENERATION;
			generation = cl_w_p->generation;
		}
		else if (cl_w_p->use_generation_gt) {
			info2 |= CL_MSG_INFO2_GENERATION_GT;
			generation = cl_w_p->generation;
		}
		else if (cl_w_p->use_generation_dup) {
			info2 |= CL_MSG_INFO2_GENERATION_DUP;
			generation = cl_w_p->generation;
		}
	}
	
	uint32_t record_ttl = cl_w_p ? cl_w_p->record_ttl : 0;
	uint32_t transaction_ttl = cl_w_p ? cl_w_p->timeout_ms : 0;
	
	// lay out the header - currently always 2, the digest array and the ns
	int n_fields = 2;
	buf = write_header(buf, msg_sz, info1, info2, 0, generation, record_ttl, transaction_ttl, n_fields, n_values);  
		
	// now the fields
	buf = write_fields_batch_digests(buf, ns, ns_len, digests, nodes, n_digests,n_my_digests, my_node);
	if (!buf) {
		if (mbuf)	free(mbuf);
		return(-1);
	}

	// lay out the ops
	if (n_values) {

		cl_msg_op *op = (cl_msg_op *) buf;
		cl_msg_op *op_tmp;
		for (i = 0; i< n_values;i++) {
			if( values ){
				value_to_op( &values[i], operator, 0, op);
			}else{
				value_to_op(0,0,&operations[i],op);
			}
	
			op_tmp = cl_msg_op_get_next(op);
			cl_msg_swap_op(op);
			op = op_tmp;
		}
	}
	return(0);	
}





#define STACK_BUF_SZ (1024 * 16) // provide a safe number for your system - linux tends to have 8M stacks these days
#define STACK_BINS 100

//
// do_batch_monte(cl_cluster *asc, const char *ns, const cf_digest *digests, const cf_node *nodes, int n_digests,
//					cf_node node, citrusleaf_get_many_cb cb, void *udata)
//
// asc - cluster to send to 
// ns - namespace for all the digests
// digests - array of digests to fetch
// nodes - array of nodes for those digests
// n_digests - size of the two preceeding arrays
// node - node of this particular request (thus will send a subset of digests)
// cb - callback that gets called back MULTITHREADED when data arrives
// udata - user data for the callback
//

static int
do_batch_monte(cl_cluster *asc, char *ns, cf_digest *digests, cl_cluster_node **nodes, int n_digests, cl_bin *bins, cl_operator operator, cl_operation *operations, int n_ops,
	cl_cluster_node *node, int n_node_digests, citrusleaf_get_many_cb cb, void *udata)
{
	int rv = -1;

	uint8_t		rd_stack_buf[STACK_BUF_SZ];	
	uint8_t		*rd_buf = 0;
	size_t		rd_buf_sz = 0;
	uint8_t		wr_stack_buf[STACK_BUF_SZ];
	uint8_t		*wr_buf = wr_stack_buf;
	size_t		wr_buf_sz = sizeof(wr_stack_buf);

	// we have a list of many keys
	int info1 = CL_MSG_INFO1_READ;
	if (0 == bins) info1 |= CL_MSG_INFO1_GET_ALL;
	rv = batch_compile(info1, 0, ns, digests, nodes, n_digests, node, n_node_digests, bins, operator, operations, n_ops, 
		&wr_buf, &wr_buf_sz, 0);
	if (rv != 0) {
		fprintf(stderr, " do batch monte: batch compile failed: some kind of intermediate error\n");
		return (rv);
	}

	
#ifdef DEBUG_VERBOSE
	dump_buf("sending request to cluster:", wr_buf, wr_buf_sz);
#endif	

	int fd = cl_cluster_node_fd_get(node, false);
	if (fd == -1) {
#ifdef DEBUG			
		fprintf(stderr, "warning: node %s has no file descriptors, retrying transaction\n",node->name);
#endif
		return(-1);
	}
	
	// send it to the cluster - non blocking socket, but we're blocking
	if (0 != cf_socket_write_forever(fd, wr_buf, wr_buf_sz)) {
#ifdef DEBUG			
		fprintf(stderr, "Citrusleaf: write timeout or error when writing header to server - %d fd %d errno %d\n",rv,fd,errno);
#endif
		return(-1);
	}

	cl_proto 		proto;
	bool done = false;
	
	do { // multiple CL proto per response
		
		// Now turn around and read a fine cl_pro - that's the first 8 bytes that has types and lenghts
		if ((rv = cf_socket_read_forever(fd, (uint8_t *) &proto, sizeof(cl_proto) ) ) ) {
			fprintf(stderr, "network error: errno %d fd %d\n",rv, fd);
			return(-1);
		}
#ifdef DEBUG_VERBOSE
		dump_buf("read proto header from cluster", (uint8_t *) &proto, sizeof(cl_proto));
#endif	
		cl_proto_swap(&proto);

		if (proto.version != PROTO_VERSION) {
			fprintf(stderr, "network error: received protocol message of wrong version %d\n",proto.version);
			return(-1);
		}
		if ((proto.type != PROTO_TYPE_CL_MSG) && (proto.type != PROTO_TYPE_CL_MSG_COMPRESSED)) {
			fprintf(stderr, "network error: received incorrect message version %d\n",proto.type);
			return(-1);
		}
		
		// second read for the remainder of the message - expect this to cover lots of data, many lines
		//
		// if there's no error
		rd_buf_sz =  proto.sz;
		if (rd_buf_sz > 0) {
                                                         
//            fprintf(stderr, "message read: size %u\n",(uint)proto.sz);
            
			if (rd_buf_sz > sizeof(rd_stack_buf))
				rd_buf = malloc(rd_buf_sz);
			else
				rd_buf = rd_stack_buf;
			if ((rv = cf_socket_read_forever(fd, rd_buf, rd_buf_sz))) {
				fprintf(stderr, "network error: errno %d fd %d\n",rv, fd);
				if (rd_buf != rd_stack_buf)	{ free(rd_buf); }
				return(-1);
			}
// this one's a little much: printing the entire body before printing the other bits			
#ifdef DEBUG_VERBOSE
			dump_buf("read msg body header (multiple msgs)", rd_buf, rd_buf_sz);
#endif	
		}
		
		if (proto.type == PROTO_TYPE_CL_MSG_COMPRESSED) {
			
			uint8_t *new_rd_buf;
			size_t  new_rd_buf_sz;
			
			rv = batch_decompress(rd_buf, rd_buf_sz, &new_rd_buf, &new_rd_buf_sz);
			if (rv != 0) {
				fprintf(stderr, "could not decompress compressed message: error %d\n",rv);
				if (rd_buf != rd_stack_buf)	{ free(rd_buf); }
			}				
				
			if (rd_buf != rd_stack_buf)	{ free(rd_buf); }
			rd_buf = new_rd_buf;
			rd_buf_sz = new_rd_buf_sz;
			
			// also re-touch the proto - not certain if this matters
			proto.sz = rd_buf_sz;
			proto.type = PROTO_TYPE_CL_MSG;

		}
		
		// process all the cl_msg in this proto
		uint8_t *buf = rd_buf;
		uint pos = 0;
		cl_bin stack_bins[STACK_BINS];
		cl_bin *bins;
		
		while (pos < rd_buf_sz) {

#ifdef DEBUG_VERBOSE
			dump_buf("individual message header", buf, sizeof(cl_msg));
#endif	
			
			uint8_t *buf_start = buf;
			cl_msg *msg = (cl_msg *) buf;
			cl_msg_swap_header(msg);
			buf += sizeof(cl_msg);
			
			if (msg->header_sz != sizeof(cl_msg)) {
				fprintf(stderr, "received cl msg of unexpected size: expecting %zd found %d, internal error\n",
					sizeof(cl_msg),msg->header_sz);
				return(-1);
			}

			// parse through the fields
			cf_digest *keyd = 0;
			char ns_ret[33] = {0};
			cl_msg_field *mf = (cl_msg_field *)buf;
			for (int i=0;i<msg->n_fields;i++) {
				cl_msg_swap_field(mf);
				if (mf->type == CL_MSG_FIELD_TYPE_KEY)
					fprintf(stderr, "read: found a key - unexpected\n");
				else if (mf->type == CL_MSG_FIELD_TYPE_DIGEST_RIPE) {
					keyd = (cf_digest *) mf->data;
				}
				else if (mf->type == CL_MSG_FIELD_TYPE_NAMESPACE) {
					memcpy(ns_ret, mf->data, cl_msg_field_get_value_sz(mf));
					ns_ret[ cl_msg_field_get_value_sz(mf) ] = 0;
				}
				mf = cl_msg_field_get_next(mf);
			}
			buf = (uint8_t *) mf;

#ifdef DEBUG_VERBOSE
			fprintf(stderr, "message header fields: nfields %u nops %u\n",msg->n_fields,msg->n_ops);
#endif


			if (msg->n_ops > STACK_BINS) {
				bins = malloc(sizeof(cl_bin) * msg->n_ops);
			}
			else {
				bins = stack_bins;
			}
			
			// parse through the bins/ops
			cl_msg_op *op = (cl_msg_op *)buf;
			for (int i=0;i<msg->n_ops;i++) {

				cl_msg_swap_op(op);

#ifdef DEBUG_VERBOSE
				fprintf(stderr, "op receive: %p size %d op %d ptype %d pversion %d namesz %d \n",
					op,op->op_sz, op->op, op->particle_type, op->version, op->name_sz);				
#endif			

#ifdef DEBUG_VERBOSE
				dump_buf("individual op (host order)", (uint8_t *) op, op->op_sz + sizeof(uint32_t));
#endif	

				set_value_particular(op, &bins[i]);
				op = cl_msg_op_get_next(op);
			}
			buf = (uint8_t *) op;
			
			if (msg->info3 & CL_MSG_INFO3_LAST)	{
#ifdef DEBUG				
				fprintf(stderr, "received final message\n");
#endif				
				done = true;
			}
			
			if (msg->n_ops) {
				// got one good value? call it a success!
				(*cb) ( ns_ret, 0 /*key*/, keyd, msg->generation, msg->record_ttl, bins, msg->n_ops, false /*islast*/, udata);
				rv = 0;
			}
//			else
//				fprintf(stderr, "received message with no bins, signal of an error\n");

			if (bins != stack_bins) {
				free(bins);
				bins = 0;
			}


			// don't have to free object internals. They point into the read buffer, where
			// a pointer is required
			pos += buf - buf_start;
			
		}
		
		if (rd_buf && (rd_buf != rd_stack_buf))	{
			free(rd_buf);
			rd_buf = 0;
		}

	} while ( done == false );

		if (wr_buf != wr_stack_buf) { 
			free(wr_buf);
			wr_buf = 0;
		}
	
	cl_cluster_node_fd_put(node, fd, false);
	
	goto Final;
	
Final:	
	
#ifdef DEBUG_VERBOSE	
	fprintf(stderr, "exited loop: rv %d\n", rv );
#endif	
	
	return(rv);
}

cf_atomic32 batch_initialized = 0;

#define N_BATCH_THREADS 6
cf_queue *g_batch_q = 0;
pthread_t g_batch_th[N_BATCH_THREADS];


//
// These externally visible functions are exposed through citrusleaf.h
//

typedef struct {
	
	// these sections are the same for the same query
	cl_cluster 	*asc; 
	char 		*ns;
	cf_digest 	*digests; 
	cl_cluster_node **nodes;
	int 		n_digests; 
	bool 		get_key;
	cl_bin 		*bins;         // Bins. If this is used, 'operation' should be null, and 'operator' should be the operation to be used on the bins
	cl_operator     operator;      // Operator.  The single operator used on all the bins, if bins is non-null
	cl_operation    *operations;   // Operations.  Set of operations (bins + operators).  Should be used if bins is not used.
	int		n_ops;          // Number of operations (count of elements in 'bins' or count of elements in 'operations', depending on which is used. 
	citrusleaf_get_many_cb cb; 
	void *udata;

	cf_queue *complete_q;
	
	// this is different for every work
	cl_cluster_node *my_node;				
	int				my_node_digest_count;
	
	int 			index; // debug only
	
} digest_work;


void *
batch_worker_fn(void *gcc_is_ass)
{
	do {
		digest_work work;
		
		if (0 != cf_queue_pop(g_batch_q, &work, CF_QUEUE_FOREVER)) {
			fprintf(stderr, "queue pop failed\n");
		}

		int an_int = do_batch_monte( work.asc, work.ns, work.digests, work.nodes, work.n_digests, work.bins, work.operator, work.operations, work.n_ops, work.my_node, work.my_node_digest_count, work.cb, work.udata );
		
		cf_queue_push(work.complete_q, (void *) &an_int);
		
	} while (1);
}

#define MAX_NODES 32

cl_rv
citrusleaf_get_many_digest(cl_cluster *asc, char *ns, const cf_digest *digests, int n_digests, cl_bin *bins, int n_bins, bool get_key, citrusleaf_get_many_cb cb, void *udata)
{
	
	// fast path: if there's only one node, or the number of digests is super short, just dispatch to the server directly

	// TODO FAST PATH

	//
	// allocate the digest-node array, and populate it
	// 
	cl_cluster_node **nodes = malloc( sizeof(cl_cluster_node *)  * n_digests);
	if (!nodes) {
		fprintf(stderr, " allocation failed ");
		return(-1);
	}
	
	// loop through all digests and determine a node
	for (int i=0;i<n_digests;i++) {
		
		nodes[i] = cl_cluster_node_get(asc, ns, &digests[i], true/*write, but that's Ok*/);
		
		// not sure if this is required - it looks like cluster_node_get automatically calls random?
		// it's certainly safer though
		if (nodes[i] == 0) {
			fprintf(stderr, "index %d: no specific node, getting random\n",i);
			nodes[i] = cl_cluster_node_get_random(asc);
		}
		if (nodes[i] == 0) {
			fprintf(stderr, "index %d: can't get any node\n", i);
			free(nodes);
			return(-1);
		}
			
	}
	// find unique set
	cl_cluster_node *unique_nodes[MAX_NODES];
	int				unique_nodes_count[MAX_NODES];
	int 			n_nodes = 0;
	for (int i=0;i<n_digests;i++) {
		// look to see if nodes[i] is in the unique list
		int j;
		for (j=0;j<n_nodes;j++) {
			if (unique_nodes[j] == nodes[i]) {
				unique_nodes_count[j]++;
				break;
			}
		}
		// not found, insert in nodes list
		if (j == n_nodes) {
			unique_nodes[n_nodes] = nodes[i];
			unique_nodes_count[n_nodes] = 1;
			n_nodes++;
		}
	}
	
	// 
	//
	digest_work work;
	work.asc = asc;
	work.ns = ns;
	work.digests = (cf_digest *) digests; // discarding const to make compiler happy
	work.nodes = nodes;
	work.n_digests = n_digests;
	work.get_key = get_key;
	work.bins = bins;
	work.operator = CL_OP_READ;
	work.operations = 0;
	work.n_ops = n_bins;
	work.cb = cb;
	work.udata = udata;
	
	work.complete_q = cf_queue_create(sizeof(int),true);
	//
	// dispatch work to the worker queue to allow the transactions in parallel
	//
	for (int i=0;i<n_nodes;i++) {
		
		// fill in per-request specifics
		work.my_node = unique_nodes[i];
		work.my_node_digest_count = unique_nodes_count[i];
		work.index = i;
		
//		fprintf(stderr,"dispatching work: index %d node %p n_digeset %d\n",i,work.my_node,work.my_node_digest_count); 
		
		// dispatch - copies data
		cf_queue_push(g_batch_q, &work);
	}
	
	// wait for the work to complete
	int retval = 0;
	for (int i=0;i<n_nodes;i++) {
		int z;
		cf_queue_pop(work.complete_q, &z, CF_QUEUE_FOREVER);
		if (z != 0)
			retval = z;
	}
	
	// free and return what needs freeing and putting
	cf_queue_destroy(work.complete_q);
	for (int i=0;i<n_digests;i++) {
		cl_cluster_node_put(nodes[i]);	
	}
	free(nodes);
	if (retval != 0) {
		return( CITRUSLEAF_FAIL_CLIENT );
	} 
	else {
		return ( 0 );
	}

}




//
// Initializes the shared thread pool and whatever queues
//

int
citrusleaf_batch_init()
{
	if (1 == cf_atomic32_incr(&batch_initialized)) {

		// create dispatch queue
		g_batch_q = cf_queue_create(sizeof(digest_work), true);
		
		// create thread pool
		for (int i=0;i<N_BATCH_THREADS;i++)
			pthread_create(&g_batch_th[i], 0, batch_worker_fn, 0);

	}
	
	return(0);	
}


