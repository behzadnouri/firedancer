#define _GNU_SOURCE
#include "configure.h"

#include "../../../tango/xdp/fd_xdp_redirect_user.h"

#include <unistd.h>
#include <sys/stat.h>
#include <linux/capability.h>
#include <linux/if_link.h>

#define NAME "xdp"

static void
init_perm( security_t *     security,
           config_t * const config ) {
  if( FD_UNLIKELY( config->development.netns.enabled ) )
    check_cap( security, NAME, CAP_SYS_ADMIN, "enter a network namespace by calling `setns(2)`" );
  else {
    check_cap( security, NAME, CAP_SYS_ADMIN, "create a BPF map with `bpf_map_create`" );
    check_cap( security, NAME, CAP_NET_ADMIN, "create an XSK map with `bpf_map_create`" );
  }
}

/* fd_xdp_redirect_prog is eBPF ELF object containing the XDP program.
   It is embedded into this program. */
FD_IMPORT_BINARY( fd_xdp_redirect_prog, "src/tango/xdp/fd_xdp_redirect_prog.o" );

static void
init( config_t * const config ) {
  if( FD_UNLIKELY( config->development.netns.enabled ) )
    enter_network_namespace( config->net.interface );

  uint mode = 0;
  if(      FD_LIKELY( !strcmp( config->net.xdp_mode, "skb" ) ) ) mode = XDP_FLAGS_SKB_MODE;
  else if( FD_LIKELY( !strcmp( config->net.xdp_mode, "drv" ) ) ) mode = XDP_FLAGS_DRV_MODE;
  else if( FD_LIKELY( !strcmp( config->net.xdp_mode, "hw"  ) ) ) mode = XDP_FLAGS_HW_MODE;
  else FD_LOG_ERR(( "unknown XDP mode `%s`", config->net.xdp_mode ));

  if( FD_UNLIKELY( fd_xdp_init( config->name,
                                0750,
                                (int)config->uid,
                                (int)config->uid ) ) )
    FD_LOG_ERR(( "fd_xdp_init failed" ));

  if( FD_UNLIKELY( fd_xdp_hook_iface( config->name,
                                      config->net.interface,
                                      mode,
                                      fd_xdp_redirect_prog,
                                      fd_xdp_redirect_prog_sz ) ) )
    FD_LOG_ERR(( "fd_xdp_hook_iface failed" ));

  /* The Linux kernel does some short circuiting optimizations
     when sending packets to an IP address that's owned by the
     same host. The optimization is basically to route them over
     to the loopback interface directly, bypassing the network
     hardware.

     This redirection to the loopback interface happens before
     XDP programs are executed, so local traffic destined for
     our listen addresses will not get ingested correctly.

     There are two reasons we send traffic locally,

      * For testing and development.
      * The Solana Labs code sends local traffic to itself to
        as part of routine operation (eg, when it's the leader
        it sends votes to its own TPU socket).

     So for now we need to also bind to loopback. This is a
     small performance hit for other traffic, but we only
     redirect packets destined for our target IP and port so
     it will not otherwise interfere. */
  if( FD_LIKELY( strcmp( config->net.interface, "lo" ) ) ) {
    if( FD_UNLIKELY( fd_xdp_hook_iface( config->name,
                                        "lo",
                                        mode,
                                        fd_xdp_redirect_prog,
                                        fd_xdp_redirect_prog_sz ) ) )
      FD_LOG_ERR(( "fd_xdp_hook_iface failed" ));
  }


  ushort udp_ports[] = { config->tiles.quic.transaction_listen_port, config->tiles.quic.quic_transaction_listen_port };
  if( FD_UNLIKELY( fd_xdp_listen_udp_ports( config->name,
                                            config->net.ip_addr,
                                            2,
                                            udp_ports,
                                            1 ) ) )
    FD_LOG_ERR(( "fd_xdp_listen_udp_ports failed" ));
}

static void
fini_perm( security_t *     security,
           config_t * const config ) {
  if( FD_UNLIKELY( config->development.netns.enabled ) )
    check_cap( security, NAME, CAP_SYS_ADMIN, "enter a network namespace by calling `setns(2)`" );
}

static void
fini( config_t * const config ) {
  if( FD_UNLIKELY( fd_xdp_fini( config->name ) ) )
    FD_LOG_ERR(( "fd_xdp_fini failed" ));

  /* work around race condition, ugly hack due to kernel maybe removing
     some hooks in the background */
  nanosleep1( 1, 0 );

  char path[ PATH_MAX ];
  snprintf1( path, PATH_MAX, "/sys/fs/bpf/%s/%s", config->name, config->net.interface );
  if( FD_UNLIKELY( rmdir( path ) && errno != ENOENT ) ) FD_LOG_ERR(( "rmdir failed (%i-%s)", errno, fd_io_strerror( errno ) ));
  snprintf1( path, PATH_MAX, "/sys/fs/bpf/%s/lo", config->name );
  if( FD_UNLIKELY( rmdir( path ) && errno != ENOENT ) ) FD_LOG_ERR(( "rmdir failed (%i-%s)", errno, fd_io_strerror( errno ) ));
  snprintf1( path, PATH_MAX, "/sys/fs/bpf/%s", config->name );
  if( FD_UNLIKELY( rmdir( path ) && errno != ENOENT ) ) FD_LOG_ERR(( "rmdir failed (%i-%s)", errno, fd_io_strerror( errno ) ));
}

static configure_result_t
check( config_t * const config ) {
  char xdp_path[ PATH_MAX ];
  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s", config->name );

  struct stat st;
  int result = stat( xdp_path, &st );
  if( FD_UNLIKELY( result && errno == ENOENT ) ) NOT_CONFIGURED( "`%s` does not exist", xdp_path );
  else if( FD_UNLIKELY( result ) ) PARTIALLY_CONFIGURED( "`%s` cannot be statted (%i-%s)", xdp_path, errno, fd_io_strerror( errno ) );

  CHECK( check_dir(  "/sys/fs/bpf", config->uid, config->uid, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP ) );
  CHECK( check_dir(  xdp_path,      config->uid, config->uid, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP ) );

  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/udp_dsts", config->name );
  CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

  char * interfaces[] = { config->net.interface, "lo" };
  ulong interfaces_sz = !strcmp( config->net.interface, "lo" ) ? 1 : 2;
  for( ulong i=0; i<interfaces_sz; i++ ) {
    snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xdp_link", config->name, interfaces[i] );
    CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

    snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xdp_prog", config->name, interfaces[i] );
    CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

    snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xsks", config->name, interfaces[i] );
    CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );
  }

  /* todo: step into these links and make sure the interior data is
           correct, eg, port numbers still match */
  CONFIGURE_OK();
}

configure_stage_t xdp = {
  .name            = NAME,
  .always_recreate = 0,
  .enabled         = NULL,
  .init_perm       = init_perm,
  .fini_perm       = fini_perm,
  .init            = init,
  .fini            = fini,
  .check           = check,
};

#undef NAME
