#include "../fd_ballet.h"
#include "fd_pack.h"
#include "fd_compute_budget_program.h"
#include "../txn/fd_txn.h"
#include <math.h>

FD_IMPORT_BINARY( sample_vote, "src/ballet/pack/sample_vote.bin" );
#define SAMPLE_VOTE_COST (3435UL)

#define MAX_TEST_TXNS (1024UL)
#define DUMMY_PAYLOAD_MAX_SZ (FD_TXN_ACCT_ADDR_SZ * 256UL + 64UL)
uchar txn_scratch[ MAX_TEST_TXNS ][ FD_TXN_MAX_SZ ];
uchar payload_scratch[ MAX_TEST_TXNS ][ DUMMY_PAYLOAD_MAX_SZ ];
ulong payload_sz[ MAX_TEST_TXNS ];

#define PACK_SCRATCH_SZ (128UL*1024UL*1024UL)
uchar pack_scratch[ PACK_SCRATCH_SZ ] __attribute__((aligned(128)));


const char SIGNATURE_SUFFIX[ FD_TXN_SIGNATURE_SZ - sizeof(ulong) - sizeof(uint) ] = ": this is the fake signature of transaction number ";
const char WORK_PROGRAM_ID[ FD_TXN_ACCT_ADDR_SZ ] = "Work Program Id Consumes 1<<j CU";

fd_rng_t _rng[1];
fd_rng_t * rng;

#define SET_NAME aset
#include "../../util/tmpl/fd_smallset.c"

struct pack_outcome {
  ulong microblock_cnt;
  aset_t  r_accts_in_use[ FD_PACK_MAX_GAP ];
  aset_t  w_accts_in_use[ FD_PACK_MAX_GAP ];
  fd_txn_p_t results[1024];
};
typedef struct pack_outcome pack_outcome_t;

pack_outcome_t outcome;


static fd_pack_t *
init_all( ulong pack_depth,
          ulong gap,
          ulong max_txn_per_microblock,
          pack_outcome_t * outcome     ) {
  ulong footprint = fd_pack_footprint( pack_depth, gap, max_txn_per_microblock );

  if( footprint>PACK_SCRATCH_SZ ) FD_LOG_ERR(( "Test required %lu bytes, but scratch was only %lu", footprint, PACK_SCRATCH_SZ ));
#if DETAILED_STATUS_MESSAGES
  else                         FD_LOG_NOTICE(( "Test required %lu bytes of %lu available bytes",    footprint, PACK_SCRATCH_SZ ));
#endif

  fd_pack_t * pack = fd_pack_join( fd_pack_new( pack_scratch, pack_depth, gap, max_txn_per_microblock, rng ) );
#define MAX_BANKING_THREADS 64

  outcome->microblock_cnt = 0UL;
  for( ulong i=0UL; i<FD_PACK_MAX_GAP; i++ ) {
    outcome->r_accts_in_use[ i ] = aset_null( );
    outcome->w_accts_in_use[ i ] = aset_null( );
  }

  return pack;
}


/* Makes enough of a transaction to schedule that reads one account for
   each character in reads and writes one account for each character in
   writes.  The characters before the nul-terminator in reads and writes
   should be in [0x30, 0x70), basically numbers and uppercase letters.
   Adds a unique signer.  Packing should estimate compute usage near the
   specified value.  Fee will be set to 5^priority, so that even with a
   large stall, it should still schedule in decreasing priority order.
   priority should be in (0, 13.5].  Stores the created transaction in
   txn_scratch[ i ] and payload_scratch[ i ].  Returns the priority fee
   in lamports. */
static ulong
make_transaction( ulong        i,
                  uint         compute,
                  double       priority,
                  char const * writes,
                  char const * reads    ) {
  uchar * p = payload_scratch[ i ];
  uchar * p_base = p;
  fd_txn_t * t = (fd_txn_t*) txn_scratch[ i ];

  *(p++) = (uchar)1;
  fd_memcpy( p,                                   &i,               sizeof(ulong)                                    );
  fd_memcpy( p+sizeof(ulong),                     SIGNATURE_SUFFIX, FD_TXN_SIGNATURE_SZ - sizeof(ulong)-sizeof(uint) );
  fd_memcpy( p+FD_TXN_SIGNATURE_SZ-sizeof(ulong), &compute,         sizeof(uint)                                     );
  p += FD_TXN_SIGNATURE_SZ;
  t->transaction_version = FD_TXN_VLEGACY;
  t->signature_cnt = 1;
  t->signature_off = 1;
  t->message_off = FD_TXN_SIGNATURE_SZ+1UL;
  t->readonly_signed_cnt = 0;
  ulong programs_to_include = 2UL; /* 1 for compute budget, 1 for "work" program */
  t->readonly_unsigned_cnt = (uchar)(strlen( reads ) + programs_to_include);
  t->acct_addr_cnt = (ushort)(1UL + strlen( reads ) + programs_to_include + strlen( writes ));

  t->acct_addr_off = FD_TXN_SIGNATURE_SZ+1UL;

  /* Add the signer */
  *p = 's' + 0x80; fd_memcpy( p+1, &i, sizeof(ulong) ); memset( p+9, 'S', 32-9 ); p += FD_TXN_ACCT_ADDR_SZ;
  /* Add the writable accounts */
  for( ulong i = 0UL; writes[i] != '\0'; i++ ) {
    memset( p, writes[i], FD_TXN_ACCT_ADDR_SZ );
    p += FD_TXN_ACCT_ADDR_SZ;
  }
  /* Add the compute budget */
  fd_memcpy( p, FD_COMPUTE_BUDGET_PROGRAM_ID, FD_TXN_ACCT_ADDR_SZ ); p += FD_TXN_ACCT_ADDR_SZ;
  /* Add the work program */
  fd_memcpy( p, WORK_PROGRAM_ID, FD_TXN_ACCT_ADDR_SZ ); p += FD_TXN_ACCT_ADDR_SZ;
  /* Add the readonly accounts */
  for( ulong i = 0UL; reads[i] != '\0'; i++ ) {
    memset( p, reads[i], FD_TXN_ACCT_ADDR_SZ );
    p += FD_TXN_ACCT_ADDR_SZ;
  }

  t->recent_blockhash_off = 0;
  t->addr_table_lookup_cnt = 0;
  t->addr_table_adtl_writable_cnt = 0;
  t->addr_table_adtl_cnt = 0;
  t->instr_cnt = (ushort)(1UL + (ulong)fd_uint_popcnt( compute ));

  uchar prog_start = (uchar)(1UL+strlen( writes ));

  t->instr[ 0 ].program_id = prog_start;
  t->instr[ 0 ].acct_cnt = 0;
  t->instr[ 0 ].data_sz = 9;
  t->instr[ 0 ].acct_off = (ushort)(p - p_base);
  t->instr[ 0 ].data_off = (ushort)(p - p_base);


  /* Write instruction data */
  uint rewards = (uint) pow( 5.0, priority );
  *p = '\0'; fd_memcpy( p+1, &compute, sizeof(uint) ); fd_memcpy( p+5, &rewards, sizeof(uint) );
  p += 9UL;

  ulong j = 1UL;
  for( uint i = 0U; i<32U; i++ ) {
    if( compute & (1U << i) ) {
      *p = (uchar)i;
      t->instr[ j ].program_id = (uchar)(prog_start + 1);
      t->instr[ j ].acct_cnt = 0;
      t->instr[ j ].data_sz = 1;
      t->instr[ j ].acct_off = (ushort)(p - p_base);
      t->instr[ j ].data_off = (ushort)(p - p_base);
      j++;
      p++;
    }
  }

  payload_sz[ i ] = (ulong)(p-p_base);
  return rewards;
}

static void
make_vote_transaction( ulong i ) {

  uchar * p = payload_scratch[ i ];

  fd_memcpy( p, sample_vote, sample_vote_sz );
  /* Make signature and the two writable accounts unique */
  p[ 0x01+(i%8) ] = (uchar)(p[ 0x01+(i%8) ] + 1UL + (i/8));
  p[ 0x45+(i%8) ] = (uchar)(p[ 0x45+(i%8) ] + 1UL + (i/8));
  p[ 0x65+(i%8) ] = (uchar)(p[ 0x65+(i%8) ] + 1UL + (i/8));
  fd_txn_parse( p, sample_vote_sz, txn_scratch[i], NULL );
}

static void
insert( ulong i,
        fd_pack_t * pack ) {
  fd_txn_p_t * slot       = fd_pack_insert_txn_init( pack );
  fd_txn_t *   txn        = (fd_txn_t*) txn_scratch[ i ];
  fd_memcpy( slot->payload, payload_scratch[ i ], payload_sz[ i ] );
  fd_memcpy( TXN(slot),     txn,     fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt ) );

  fd_pack_insert_txn_fini( pack, slot );
}



static void
schedule_validate_microblock( fd_pack_t * pack,
                              ulong total_cus,
                              float vote_fraction,
                              ulong min_txns,
                              ulong min_rewards,
                              pack_outcome_t * outcome ) {

  ulong pre_txn_cnt  = fd_pack_avail_txn_cnt( pack );
  ulong txn_cnt = fd_pack_schedule_next_microblock( pack, total_cus, vote_fraction, outcome->results );
  ulong post_txn_cnt = fd_pack_avail_txn_cnt( pack );

#if DETAILED_STATUS_MESSAGES
  FD_LOG_NOTICE(( "Scheduling microblock. %lu avail -> %lu avail. %lu scheduled", pre_txn_cnt, post_txn_cnt, txn_cnt ));
#endif

  FD_TEST( txn_cnt >= min_txns );
  FD_TEST( pre_txn_cnt-post_txn_cnt == txn_cnt );

  ulong total_rewards = 0UL;


  aset_t  read_accts = aset_null( );
  aset_t write_accts = aset_null( );

  for( ulong i=0UL; i<txn_cnt; i++ ) {
    fd_txn_p_t * txnp = outcome->results+i;
    fd_txn_t   * txn  = TXN(txnp);

    fd_compute_budget_program_state_t cbp;
    fd_compute_budget_program_init( &cbp );

    ulong rewards = 0UL;
    uint compute = 0U;
    if( FD_LIKELY( txn->instr_cnt>1UL ) ) {
      fd_txn_instr_t ix = txn->instr[0]; /* For these transactions, the compute budget instr is always the 1st */
      FD_TEST( fd_compute_budget_program_parse( txnp->payload + ix.data_off, ix.data_sz, &cbp ) );
      fd_compute_budget_program_finalize( &cbp, txn->instr_cnt, &rewards, &compute );
    } /* else it's a vote */

    total_rewards += rewards;

    fd_acct_addr_t const * acct = fd_txn_get_acct_addrs( txn, txnp->payload );
    fd_txn_acct_iter_t ctrl[1];
    for( ulong j=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_WRITABLE_NONSIGNER_IMM, ctrl ); j<fd_txn_acct_iter_end();
        j = fd_txn_acct_iter_next( j, ctrl ) ) {
      uchar b0 = acct[j].b[0]; uchar b1 = acct[j].b[1];
      if( (0x30UL<=b0) & (b0<0x70UL) & (b0==b1) ) {
        FD_TEST( !aset_test( write_accts, (ulong)b0-0x30 ) );
        write_accts = aset_insert( write_accts, (ulong)b0-0x30UL );
      }
    }
    for( ulong j=fd_txn_acct_iter_init( txn, FD_TXN_ACCT_CAT_READONLY_NONSIGNER_IMM, ctrl ); j<fd_txn_acct_iter_end();
        j = fd_txn_acct_iter_next( j, ctrl ) ) {
      uchar b0 = acct[j].b[0]; uchar b1 = acct[j].b[1];
      if( (0x30UL<=b0) & (b0<0x70UL) & (b0==b1) )
        read_accts = aset_insert( read_accts, (ulong)b0-0x30UL );
    }
  }

  FD_TEST( total_rewards >= min_rewards );

  FD_TEST( aset_is_null( aset_intersect( read_accts, write_accts ) ) );

  /* Check for conflict with gap back */
  for( ulong i=1UL; i<fd_ulong_min( outcome->microblock_cnt, fd_pack_gap( pack ) ); i++ ) {
    ulong mb = (outcome->microblock_cnt - i) % FD_PACK_MAX_GAP;

    FD_TEST( aset_is_null( aset_intersect( write_accts, outcome->r_accts_in_use[ mb ] ) ) );
    FD_TEST( aset_is_null( aset_intersect( write_accts, outcome->w_accts_in_use[ mb ] ) ) );
    FD_TEST( aset_is_null( aset_intersect( read_accts,  outcome->w_accts_in_use[ mb ] ) ) );
  }
  ulong mb = outcome->microblock_cnt % FD_PACK_MAX_GAP;
  outcome->r_accts_in_use[ mb ] =  read_accts;
  outcome->w_accts_in_use[ mb ] = write_accts;

  outcome->microblock_cnt++;
}

void test0( void ) {
  FD_LOG_NOTICE(( "TEST 0" ));
  fd_pack_t * pack = init_all( 128UL, 3UL, 128UL, &outcome );
  ulong i = 0UL;
  ulong rewards = 0UL;
  rewards += make_transaction( i,  500U, 11.0, "A",    "B" ); insert( i++, pack );
  rewards += make_transaction( i,  500U, 10.0, "C",    "D" ); insert( i++, pack );
  rewards += make_transaction( i,  800U, 10.0, "EFGH", "D" ); insert( i++, pack );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 3UL, rewards, &outcome );

  make_transaction( i,  500U, 10.0, "D", "I" );    insert( i++, pack );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 0UL, 0UL, &outcome ); /* Can't schedule due to gap */
  schedule_validate_microblock( pack, 30000UL, 0.0f, 0UL, 0UL, &outcome ); /* Can't schedule due to gap */
  schedule_validate_microblock( pack, 30000UL, 0.0f, 1UL, 0UL, &outcome ); /* Gap ended */
}

/* The original two that broke my first algorithm */
void test1( void ) {
  FD_LOG_NOTICE(( "TEST 1" ));
  fd_pack_t * pack = init_all( 128UL, 1UL, 128UL, &outcome );
  ulong i = 0;
  ulong reward1 = make_transaction( i,  500U, 11.0, "A", "B" ); insert( i++, pack );
  ulong reward2 = make_transaction( i,  500U, 10.0, "B", "A" ); insert( i++, pack );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 1UL, reward1, &outcome );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 1UL, reward2, &outcome );
}

void test2( void ) {
  FD_LOG_NOTICE(( "TEST 2" ));
  fd_pack_t * pack = init_all( 128UL, 1UL, 128UL, &outcome );
  ulong i = 0;
  double j = 13.0;
  ulong r0 = make_transaction( i,  500U, j--, "B", "A" ); insert( i++, pack );
  ulong r1 = make_transaction( i,  500U, j--, "C", "B" ); insert( i++, pack );
  ulong r2 = make_transaction( i,  500U, j--, "D", "C" ); insert( i++, pack );
  ulong r3 = make_transaction( i,  500U, j--, "A", "D" ); insert( i++, pack );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 2UL, r0+r2, &outcome );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 2UL, r1+r3, &outcome );

  /* A smart scheduler that allows read bypass could schedule the first 3 at
     the same time then #4 after they all finish. */
}

void test_vote( void ) {
  FD_LOG_NOTICE(( "TEST VOTE" ));
  fd_pack_t * pack = init_all( 128UL, 1UL, 4UL, &outcome );
  ulong i = 0;

  make_vote_transaction( i ); insert( i++, pack );
  make_vote_transaction( i ); insert( i++, pack );
  make_vote_transaction( i ); insert( i++, pack );
  make_vote_transaction( i ); insert( i++, pack );

  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 4UL );
  schedule_validate_microblock( pack, 30000UL, 0.0f, 0UL, 0UL, &outcome );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 4UL );

  schedule_validate_microblock( pack, 30000UL, 0.25f, 1UL, 0UL, &outcome );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 3UL );

  schedule_validate_microblock( pack, 30000UL, 1.0f, 3UL, 0UL, &outcome );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 0UL );

  for( ulong j=0UL; j<3UL; j++ ) FD_TEST( outcome.results[ j ].is_simple_vote );
}

static void
test_delete( void ) {
  ulong i = 0UL;
  FD_LOG_NOTICE(( "TEST DELETE" ));
  fd_pack_t * pack = init_all( 10240UL, 4UL, 128UL, &outcome );

  make_transaction( i, 800U, 12.0, "A", "B" ); insert( i++, pack );
  make_transaction( i, 700U, 11.0, "C", "D" ); insert( i++, pack );
  make_transaction( i, 600U, 10.0, "E", "F" ); insert( i++, pack );
  make_transaction( i, 500U,  9.0, "G", "H" ); insert( i++, pack );
  make_transaction( i, 400U,  8.0, "I", "J" ); insert( i++, pack );
  make_transaction( i, 300U,  7.0, "K", "L" ); insert( i++, pack );

  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 6UL );

  fd_ed25519_sig_t const * sig0 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[0], payload_scratch[0] );
  fd_ed25519_sig_t const * sig2 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[2], payload_scratch[2] );
  fd_ed25519_sig_t const * sig4 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[4], payload_scratch[4] );

  FD_TEST( fd_pack_delete_transaction( pack, sig0 ) );  FD_TEST( !fd_pack_delete_transaction( pack, sig0 ) );
  FD_TEST( fd_pack_delete_transaction( pack, sig2 ) );  FD_TEST( !fd_pack_delete_transaction( pack, sig2 ) );
  FD_TEST( fd_pack_delete_transaction( pack, sig4 ) );  FD_TEST( !fd_pack_delete_transaction( pack, sig4 ) );

  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 3UL );

  schedule_validate_microblock( pack, 300000UL, 0.0f, 3UL, 0UL, &outcome );

  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 0UL );

  fd_ed25519_sig_t const * sig1 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[1], payload_scratch[1] );
  fd_ed25519_sig_t const * sig3 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[3], payload_scratch[3] );
  fd_ed25519_sig_t const * sig5 = fd_txn_get_signatures( (fd_txn_t *)txn_scratch[5], payload_scratch[5] );

  /* transactions 1,3,5 were scheduled so now deleting them fails */
  FD_TEST( !fd_pack_delete_transaction( pack, sig1 ) );
  FD_TEST( !fd_pack_delete_transaction( pack, sig3 ) );
  FD_TEST( !fd_pack_delete_transaction( pack, sig5 ) );

  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 0UL );

  i=0UL;
  ulong r0 = make_transaction( i, 800U, 12.0, "A", "B" ); insert( i++, pack );
  /*      */ make_transaction( i, 700U, 11.0, "A", "D" ); insert( i++, pack );
  ulong r2 = make_transaction( i, 600U, 10.0, "A", "F" ); insert( i++, pack );
  /*      */ make_transaction( i, 500U,  9.0, "A", "H" ); insert( i++, pack );
  /*      */ make_transaction( i, 400U,  8.0, "A", "J" ); insert( i++, pack );
  /*      */ make_transaction( i, 300U,  7.0, "A", "L" ); insert( i++, pack );

  /* They all conflict now */

  schedule_validate_microblock( pack, 300000UL, 0.0f, 1UL, r0, &outcome );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 5UL );

  FD_TEST( !fd_pack_delete_transaction( pack, sig0 ) );
  FD_TEST(  fd_pack_delete_transaction( pack, sig1 ) );
  FD_TEST(  fd_pack_delete_transaction( pack, sig5 ) );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 3UL );

  /* wait the gap */
  schedule_validate_microblock( pack, 300000UL, 0.0f, 0UL, 0, &outcome );
  schedule_validate_microblock( pack, 300000UL, 0.0f, 0UL, 0, &outcome );
  schedule_validate_microblock( pack, 300000UL, 0.0f, 0UL, 0, &outcome );

  schedule_validate_microblock( pack, 300000UL, 0.0f, 1UL, r2, &outcome );
  FD_TEST(  fd_pack_delete_transaction( pack, sig3 ) );
  FD_TEST(  fd_pack_delete_transaction( pack, sig4 ) );
  FD_TEST( fd_pack_avail_txn_cnt( pack ) == 0UL );
}

void performance_test( void ) {
  ulong i = 0UL;
  FD_LOG_NOTICE(( "TEST PERFORMANCE" ));
  fd_pack_t * pack = init_all( 10240UL, 1UL, 2UL, &outcome );
  make_transaction( i,   800U, 12.0, "ABC", "DEF" );
  make_transaction( i+1, 500U, 12.0, "GHJ", "KLMNOP" );

  long start = fd_log_wallclock( );
  for( ulong j=0UL; j<1024UL; j++ ) {
    payload_scratch[j&1][ 1+(j%8) ]++;
    fd_txn_p_t * slot       = fd_pack_insert_txn_init( pack );
    fd_txn_t *   txn        = (fd_txn_t*) txn_scratch[ j&1 ];
    fd_memcpy( slot->payload, payload_scratch[ j&1 ], payload_sz[ j&1 ]                                              );
    fd_memcpy( TXN(slot),     txn,                    fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt ) );

    fd_pack_insert_txn_fini( pack, slot );
  }
  long end = fd_log_wallclock( );
  FD_LOG_NOTICE(( "Inserting when not full: %f ns", ((double)(end-start))/10240.0 ));
  start = fd_log_wallclock( );
  for( ulong j=0UL; j<10240UL; j++ ) {
    fd_pack_schedule_next_microblock( pack, 2000UL, 0.0f, outcome.results );
  }
  end = fd_log_wallclock( );
  FD_LOG_NOTICE(( "Scheduling: %f ns", ((double)(end-start))/10240.0 ));
}


void heap_overflow_test( void ) {
  FD_LOG_NOTICE(( "TEST HEAP OVERFLOW" ));
  fd_pack_t * pack = init_all( 1024UL, 1UL, 2UL, &outcome );
  /* Insert a bunch of low-paying transactions */
  for( ulong j=0UL; j<1024UL; j++ ) {
    make_transaction( j, 800U, 4.0, "ABC", "DEF" );
    fd_txn_p_t * slot       = fd_pack_insert_txn_init( pack );
    fd_txn_t *   txn        = (fd_txn_t*) txn_scratch[ j ];
    fd_memcpy( slot->payload, payload_scratch[ j ], payload_sz[ j ]                                                );
    fd_memcpy( TXN(slot),     txn,                  fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt ) );

    fd_pack_insert_txn_fini( pack, slot );
  }
  FD_TEST( fd_pack_avail_txn_cnt( pack )==1024UL );

  /* Now insert higher-paying transactions. They should mostly take the place
     of the low-paying transactions */
  ulong r_hi = make_transaction( 1UL, 500U, 10.0, "GHJ", "KLMNOP" );
  for( ulong j=0UL; j<1024UL; j++ ) {
    payload_scratch[1][ 1+(j%8) ]++;
    fd_txn_p_t * slot       = fd_pack_insert_txn_init( pack );
    fd_txn_t *   txn        = (fd_txn_t*) txn_scratch[ 1UL ];
    fd_memcpy( slot->payload, payload_scratch[ 1UL ], payload_sz[ 1UL ]                                              );
    fd_memcpy( TXN(slot),     txn,                    fd_txn_footprint( txn->instr_cnt, txn->addr_table_lookup_cnt ) );

    fd_pack_insert_txn_fini( pack, slot );
  }

  FD_TEST( fd_pack_avail_txn_cnt( pack )==1024UL );

  for( ulong j=0UL; j<1024UL; j++ ) {
    schedule_validate_microblock( pack, 10000UL, 0.0f, 1UL, r_hi, &outcome );
  }

  FD_TEST( fd_pack_avail_txn_cnt( pack )==0UL );
}

static void
test_gap( void ) {
  FD_LOG_NOTICE(( "TEST GAP" ));

  for( ulong gap=1UL; gap<=FD_PACK_MAX_GAP; gap++ ) {
    fd_pack_t * pack = init_all( 10240UL, gap, 2UL, &outcome );

    ulong i=0UL;
    ulong reward1 = make_transaction( i,  500U, 11.0, "A", "B" );      insert( i++, pack );
    ulong reward2 = make_transaction( i,  500U, 10.0, "B", "A" );      insert( i++, pack );

    schedule_validate_microblock( pack, 10000UL, 0.0f, 1UL, reward1, &outcome );

    for( ulong j=1UL; j<gap; j++ ) schedule_validate_microblock( pack, 10000UL, 0.0f, 0UL, 0UL, &outcome );

    FD_TEST( fd_pack_avail_txn_cnt( pack )==1UL );

    schedule_validate_microblock( pack, 10000UL, 0.0f, 1UL, reward2, &outcome );
  }
}

static void
test_limits( void ) {
  FD_LOG_NOTICE(( "TEST LIMITS" ));

  /* Test the max txn per microblock limit */
  for( ulong max=1UL; max<=15UL; max++ ) {
    fd_pack_t * pack = init_all( 1024UL, 1UL, max, &outcome );

    for( ulong i=0UL; i<max*2UL; i++ ) {
      /* The votes are all non-conflicting */
      make_vote_transaction( i );
      insert( i, pack );
    }
    FD_TEST( fd_pack_avail_txn_cnt( pack )==max*2UL );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 1.0f, max, 0UL, &outcome );
    FD_TEST( fd_pack_avail_txn_cnt( pack )==max     );
  }


  /* Test the CU limit */
  if( 1 ) {
    fd_pack_t * pack = init_all( 1024UL, 1UL, 1024UL, &outcome );

    for( ulong i=0UL; i<1024UL; i++ ) {
      /* The votes are all non-conflicting */
      make_vote_transaction( i );
      insert( i, pack );
    }
    for( ulong cu_limit=0UL; cu_limit<45UL*SAMPLE_VOTE_COST; cu_limit += SAMPLE_VOTE_COST ) {
      /* FIXME: CU limit for votes is done based on the typical cost,
         which is slightly different from the sample vote cost. */
      schedule_validate_microblock( pack, cu_limit*3437/SAMPLE_VOTE_COST, 1.0f, cu_limit/SAMPLE_VOTE_COST, 0UL, &outcome );
    }
    /* sum_{x=0}^44 x = 990, so there should be 34 transactions left */
    FD_TEST( fd_pack_avail_txn_cnt( pack )==34UL );
  }


  /* Test the block vote limit */
  if( 1 ) {
    fd_pack_t * pack = init_all( 1024UL, 1UL, 1024UL, &outcome );

    for( ulong j=0UL; j<FD_PACK_MAX_VOTE_COST_PER_BLOCK/(1024UL*SAMPLE_VOTE_COST); j++ ) {
      for( ulong i=0UL; i<1024UL; i++ ) { make_vote_transaction( i ); insert( i, pack ); }
      schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 1.0f, 1024UL, 0UL, &outcome );
    }

    for( ulong i=0UL; i<1024UL; i++ ) { make_vote_transaction( i ); insert( i, pack ); }
    ulong consumed_cost = (1024UL*SAMPLE_VOTE_COST)*(FD_PACK_MAX_VOTE_COST_PER_BLOCK/(1024UL*SAMPLE_VOTE_COST));
    ulong expected_votes = (FD_PACK_MAX_VOTE_COST_PER_BLOCK-consumed_cost)/SAMPLE_VOTE_COST;

    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 1.0f,        expected_votes, 0UL, &outcome );
    FD_TEST( fd_pack_avail_txn_cnt( pack )==1024UL-expected_votes );

    fd_pack_end_block( pack );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 1.0f, 1024UL-expected_votes, 0UL, &outcome );
  }


  /* Test the block writer limit */
  if( 1 ) {
    fd_pack_t * pack = init_all( 1024UL, 1UL, 1024UL, &outcome );
    /* The limit is based on cost units, and make_transaction takes just
       compute CUs.  Add the +1 to force the rounding to make these
       close enough. */
    for( ulong j=0UL; j<FD_PACK_MAX_WRITE_COST_PER_ACCT/1000001UL; j++ ) {
      make_transaction( 0UL, 1000001U, 11.0, "A", "B" );
      insert( 0UL, pack );
      schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 1UL, 0UL, &outcome );
    }

    make_transaction( 0UL, 1000001U, 11.0, "A", "B" );
    insert( 0UL, pack );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 0UL, 0UL, &outcome );
    FD_TEST( fd_pack_avail_txn_cnt( pack )==1UL );

    fd_pack_end_block( pack );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 1UL, 0UL, &outcome );
  }


  /* Test the total cost block limit */
  if( 1 ) {
    fd_pack_t * pack = init_all( 1024UL, 1UL, 1024UL, &outcome );
    /* The limit is based on cost units, and make_transaction takes just
       compute CUs.  Add the +1 to force the rounding to make these
       close enough. */
    ulong i=0UL;
    for( ulong j=0UL; j<FD_PACK_MAX_COST_PER_BLOCK/4000004UL; j++ ) {
      make_transaction( i, 1000001U, 11.0, "A", "B" );     insert( i++, pack );
      make_transaction( i, 1000001U, 11.0, "C", "D" );     insert( i++, pack );
      make_transaction( i, 1000001U, 11.0, "E", "F" );     insert( i++, pack );
      make_transaction( i, 1000001U, 11.0, "G", "H" );     insert( i++, pack );
      schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 4UL, 0UL, &outcome );
    }

    make_transaction( i, 1000001U, 11.0, "J", "K" );     insert( i++, pack );
    make_transaction( i, 1000001U, 11.0, "L", "M" );     insert( i++, pack );
    make_transaction( i, 1000001U, 11.0, "N", "P" );     insert( i++, pack );
    make_transaction( i, 1000001U, 10.0, "Q", "R" );     insert( i++, pack );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 3UL, 0UL, &outcome );
    FD_TEST( fd_pack_avail_txn_cnt( pack )==1UL );

    fd_pack_end_block( pack );
    schedule_validate_microblock( pack, FD_PACK_MAX_COST_PER_BLOCK, 0.0f, 1UL, 0UL, &outcome );
  }
}


int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );
  rng = fd_rng_join( fd_rng_new( _rng, 0U, 0UL ) );

  test0();
  test1();
  test2();
  test_vote();
  performance_test();
  heap_overflow_test();
  test_delete();
  test_gap();
  test_limits();

  fd_rng_delete( fd_rng_leave( rng ) );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
