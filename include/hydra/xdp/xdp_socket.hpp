#pragma once

#ifdef ENABLE_AFXDP

// include/hydra/xdp/xdp_socket.hpp
//
// HYDRA-LOB Phase 10 -- AF_XDP zero-copy RX socket (declaration).
//
// Declares XdpSocket: one AF_XDP socket bound to a single {netdev, queue}
// pair -- the shared UMEM region and all four libbpf/libxdp rings (FILL,
// COMPLETION, RX, TX). Method bodies live in net/xdp_socket_user.cpp,
// compiled into blitz_lob only when ENABLE_AFXDP is ON (see
// CMakeLists.txt's target_sources(blitz_lob PRIVATE net/xdp_socket_user.cpp)
// under the ENABLE_AFXDP block) -- matching the master roadmap's Phase 1
// CMakeLists.txt spec, which describes AF_XDP support as "net/-adjacent XDP
// sources" compiled into the release binary under a compile condition, not
// a separate target.
//
// recv_batch() is the hot-path entry point a future rx_thread_fn
// (pipeline.hpp/pipeline.cpp) will drain in a tight busy-poll loop once
// Phase 10 is wired into the pipeline.
//
// This entire file compiles to nothing when ENABLE_AFXDP is undefined.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#include "hydra/config.hpp"

namespace hydra::xdp
{

    // A single received frame handed back by XdpSocket::recv_batch(): the
    // UMEM byte offset and length of one packet. `addr` is opaque to the
    // caller -- pass it to XdpSocket::frame_data() to read bytes, and to
    // XdpSocket::release_frame() exactly once when done with it.
    struct FrameDescriptor
    {
        uint64_t addr;
        uint32_t len;
    };

    // Owns one AF_XDP socket: UMEM + FILL/COMPLETION/RX/TX rings, bound to
    // one {netdev, queue} pair. recv_batch() is busy-poll only -- no
    // poll(), epoll_wait(), or any blocking/sleeping syscall -- because this
    // class is meant to sit on the hot ingress path of the matching
    // pipeline and must never let the kernel put the RX thread to sleep.
    //
    // WHY veth vs a real NIC: veth interfaces only support AF_XDP's
    // XDP_COPY mode -- there's no physical DMA engine behind a veth peer to
    // own UMEM buffers directly, so the kernel copies each frame into the
    // UMEM itself. True XDP_ZEROCOPY requires a NIC + driver pair that
    // implements the AF_XDP zero-copy datapath (e.g. Mellanox mlx5, Intel
    // i40e/ice). A dev-box test against a veth pair (scripts/setup_veth.sh)
    // validates ring-plumbing correctness -- it is NOT a zero-copy
    // performance validation, and its latency numbers must never be quoted
    // as such.
    //
    // WHY plain (non-atomic) free-frame allocator: exactly one thread owns
    // and drives a given XdpSocket (the future RX thread), matching this
    // codebase's single-owner-per-hot-path-object wiring policy -- see
    // ObjectPool's equivalent WHY comment (object_pool.hpp) -- so no
    // cross-thread synchronization is needed for the free-frame stack.
    class XdpSocket
    {
    public:
        // ifname:      interface to bind to, e.g. "veth0" or "eth0".
        // queue_id:    hardware RX queue index this socket receives from.
        // umem_size:   total UMEM region size in bytes; must be a multiple
        //              of hydra::config::AFXDP_FRAME_SIZE and should equal
        //              hydra::config::AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE
        //              for the free-frame allocator to cover the whole
        //              UMEM.
        // xsks_map_fd: fd of a pre-loaded, pre-attached "xsks_map" BPF map
        //              (see load_and_attach_xdp_program()/
        //              find_xsks_map_fd() below). When >= 0, the socket
        //              runs in "custom program" mode
        //              (XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD) and registers
        //              itself into that map -- this is the mode used with
        //              net/xdp_prog.bpf.c. When -1 (default), libxdp loads
        //              and manages its own default redirect-everything XDP
        //              program instead, bypassing net/xdp_prog.bpf.c's UDP
        //              filter.
        //
        // Setup-time only: throws std::runtime_error on any failure
        // (interface not found, UMEM/socket creation failure,
        // RLIMIT_MEMLOCK not raisable). Never called from the hot path, so
        // throwing here is consistent with the rest of this codebase's
        // error-handling policy (see affinity.hpp's verify_affinity()).
        XdpSocket(const std::string &ifname, uint32_t queue_id,
                  std::size_t umem_size, int xsks_map_fd = -1);

        ~XdpSocket();

        XdpSocket(const XdpSocket &) = delete;
        XdpSocket &operator=(const XdpSocket &) = delete;
        XdpSocket(XdpSocket &&) = delete;
        XdpSocket &operator=(XdpSocket &&) = delete;

        // Busy-poll receive: drains up to `max` frames from the RX ring
        // into `out` without blocking, topping up the FILL ring first so
        // the kernel always has somewhere to land the next DMA. Returns
        // the number of frames received -- 0 is the normal "nothing
        // pending" result, not an error; callers are expected to spin.
        // Ownership of each returned frame transfers to the caller, who
        // must eventually call release_frame() on its `addr` exactly once
        // (see zero_copy_parser.hpp's parse_and_release() for the typical
        // parse-then-release pairing).
        [[nodiscard]] std::size_t recv_batch(FrameDescriptor *out, std::size_t max) noexcept;

        // Raw pointer to the first byte of frame `addr` inside the UMEM.
        // Zero-copy: callers (zero_copy_parser.hpp) read packet fields
        // straight through this pointer -- never staged through an
        // intermediate copy.
        [[nodiscard]] uint8_t *frame_data(uint64_t addr) noexcept;

        // Returns a consumed UMEM frame to the free-frame pool so a future
        // recv_batch() call can restuff it into the FILL ring for the
        // kernel to DMA into again. Must be called exactly once per frame
        // handed out by recv_batch(), after the caller is done reading it.
        // A call that would exceed this instance's actual frame count (a
        // double-release, or a release of an address this instance never
        // owned) is dropped rather than corrupting the free-list, and
        // counted -- see double_release_count().
        void release_frame(uint64_t addr) noexcept;

        [[nodiscard]] int fd() const noexcept;

        // Count of release_frame() calls dropped because the free-list was
        // already at this instance's full frame count -- i.e. a caller bug
        // (double-release), not a normal operating condition. Should stay
        // 0 always; non-zero means an invariant was violated somewhere
        // upstream (mirrors ObjectPool::exhaustion_count()'s role for the
        // pool allocator -- see object_pool.hpp).
        [[nodiscard]] uint64_t double_release_count() const noexcept
        {
            return double_release_count_;
        }

    private:
        void refill_fill_ring() noexcept;

        void *umem_area_ = nullptr;
        std::size_t umem_size_ = 0;

        xsk_umem *umem_ = nullptr;
        xsk_ring_prod fill_ring_{};
        xsk_ring_cons completion_ring_{};
        xsk_ring_cons rx_ring_{};
        xsk_ring_prod tx_ring_{};
        xsk_socket *socket_ = nullptr;

        // Set once in the constructor to umem_size_ / AFXDP_FRAME_SIZE --
        // this instance's actual seeded frame count, which release_frame()
        // must guard against instead of free_frames_.size() (the
        // compile-time array capacity): a umem_size smaller than
        // AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE (the header above documents
        // this as a legal, if suboptimal, construction) would otherwise let
        // a real double-release go undetected until free_frame_count_
        // exceeded the array's full compile-time capacity rather than this
        // instance's true frame count.
        std::size_t num_frames_ = 0;

        std::array<uint64_t, hydra::config::AFXDP_NUM_FRAMES> free_frames_{};
        std::size_t free_frame_count_ = 0;
        uint64_t double_release_count_ = 0;
    };

    // --- XDP program loading helpers (setup-time only, never on the hot path) ---
    //
    // Wrap net/xdp_prog.bpf.c's lifecycle: load the compiled ELF, attach it
    // to an interface, hand XdpSocket the "xsks_map" fd it needs to
    // register into, and detach cleanly at shutdown.

    // Loads and attaches the XDP program at `bpf_object_path` (the output
    // of net/xdp_prog.bpf.c) to `ifname`. Throws std::runtime_error on any
    // failure. The returned xdp_program* must be released via
    // detach_xdp_program() at shutdown.
    [[nodiscard]] xdp_program *load_and_attach_xdp_program(
        const std::string &ifname, const std::string &bpf_object_path,
        const std::string &section_name = "xdp",
        xdp_attach_mode mode = XDP_MODE_NATIVE);

    // Looks up the "xsks_map" BPF map fd inside an already-loaded XDP
    // program. Throws if the ELF doesn't export a map by that name.
    [[nodiscard]] int find_xsks_map_fd(xdp_program *prog);

    // Detaches and releases a program obtained from
    // load_and_attach_xdp_program(). Teardown-time only; safe to call with
    // prog == nullptr.
    void detach_xdp_program(xdp_program *prog, const std::string &ifname,
                             xdp_attach_mode mode = XDP_MODE_NATIVE) noexcept;

} // namespace hydra::xdp

#endif // ENABLE_AFXDP
