// net/xdp_socket_user.cpp
//
// HYDRA-LOB Phase 10 -- AF_XDP zero-copy RX socket (implementation).
//
// Method bodies for hydra::xdp::XdpSocket (declared in
// include/hydra/xdp/xdp_socket.hpp) plus the XDP-program load/attach
// helpers it depends on. Only compiled into blitz_lob when ENABLE_AFXDP is
// ON (see CMakeLists.txt) -- with ENABLE_AFXDP off, this TU isn't part of
// the build at all, and xdp_socket.hpp's own #ifdef guard means there is
// nothing here to compile against anyway.
//
// The libbpf/libxdp API usage below (xsk_umem__create, xsk_socket__create,
// the FILL/COMPLETION/RX/TX ring functions, the xsks_map custom-program
// pattern) mirrors the xdp-tutorial project's advanced03-AF_XDP example
// (af_xdp_user.c) -- the reference implementation this class is built on
// top of, wrapped in RAII instead of hand-rolled C, with an actual UDP
// filter in net/xdp_prog.bpf.c instead of an every-other-packet demo
// counter.

#include "hydra/xdp/xdp_socket.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <net/if.h>
#include <sys/resource.h>
#include <unistd.h>

#include <bpf/bpf.h>

namespace hydra::xdp
{

    namespace
    {
        static_assert(hydra::config::AFXDP_FRAME_SIZE == XSK_UMEM__DEFAULT_FRAME_SIZE,
                      "hydra::config::AFXDP_FRAME_SIZE must track libxdp's "
                      "XSK_UMEM__DEFAULT_FRAME_SIZE -- UMEM offset math "
                      "assumes they're identical");
    } // namespace

    XdpSocket::XdpSocket(const std::string &ifname, uint32_t queue_id,
                          std::size_t umem_size, int xsks_map_fd)
        : umem_size_(umem_size)
    {
        if (umem_size_ % hydra::config::AFXDP_FRAME_SIZE != 0)
        {
            throw std::runtime_error(
                "XdpSocket: umem_size must be a multiple of AFXDP_FRAME_SIZE");
        }
        const std::size_t num_frames = umem_size_ / hydra::config::AFXDP_FRAME_SIZE;
        if (num_frames > hydra::config::AFXDP_NUM_FRAMES)
        {
            throw std::runtime_error(
                "XdpSocket: umem_size exceeds AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE");
        }

        // Allow unlimited locked memory so the UMEM region (DMA target for
        // the NIC) can be pinned. Best-effort + throw on failure: this is
        // setup-time, not hot-path.
        rlimit rlim{RLIM_INFINITY, RLIM_INFINITY};
        if (setrlimit(RLIMIT_MEMLOCK, &rlim) != 0)
        {
            throw std::runtime_error(
                std::string("XdpSocket: setrlimit(RLIMIT_MEMLOCK) failed: ") +
                std::strerror(errno));
        }

        if (posix_memalign(&umem_area_, static_cast<std::size_t>(getpagesize()),
                            umem_size_) != 0 ||
            umem_area_ == nullptr)
        {
            throw std::runtime_error("XdpSocket: posix_memalign for UMEM failed");
        }

        int ret = xsk_umem__create(&umem_, umem_area_, umem_size_, &fill_ring_,
                                    &completion_ring_, nullptr);
        if (ret != 0)
        {
            std::free(umem_area_);
            throw std::runtime_error(
                std::string("XdpSocket: xsk_umem__create failed: ") +
                std::strerror(-ret));
        }

        xsk_socket_config xsk_cfg{};
        xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
        xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
        xsk_cfg.xdp_flags = 0;
        xsk_cfg.bind_flags = 0;
        xsk_cfg.libxdp_flags =
            (xsks_map_fd >= 0) ? XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD : 0;

        ret = xsk_socket__create(&socket_, ifname.c_str(), queue_id, umem_,
                                  &rx_ring_, &tx_ring_, &xsk_cfg);
        if (ret != 0)
        {
            xsk_umem__delete(umem_);
            std::free(umem_area_);
            throw std::runtime_error(
                std::string("XdpSocket: xsk_socket__create failed on '") +
                ifname + "' queue " + std::to_string(queue_id) + ": " +
                std::strerror(-ret));
        }

        if (xsks_map_fd >= 0)
        {
            ret = xsk_socket__update_xskmap(socket_, xsks_map_fd);
            if (ret != 0)
            {
                xsk_socket__delete(socket_);
                xsk_umem__delete(umem_);
                std::free(umem_area_);
                throw std::runtime_error(
                    std::string("XdpSocket: xsk_socket__update_xskmap failed: ") +
                    std::strerror(-ret));
            }
        }

        // Seed the free-frame allocator with every frame in the UMEM, then
        // stuff as many as possible into the FILL ring so the kernel has
        // somewhere to DMA the first packets.
        for (std::size_t i = 0; i < num_frames; ++i)
        {
            free_frames_[free_frame_count_++] =
                i * hydra::config::AFXDP_FRAME_SIZE;
        }
        refill_fill_ring();
    }

    XdpSocket::~XdpSocket()
    {
        if (socket_ != nullptr)
        {
            xsk_socket__delete(socket_);
        }
        if (umem_ != nullptr)
        {
            xsk_umem__delete(umem_);
        }
        if (umem_area_ != nullptr)
        {
            std::free(umem_area_);
        }
    }

    std::size_t XdpSocket::recv_batch(FrameDescriptor *out, std::size_t max) noexcept
    {
        refill_fill_ring();

        uint32_t idx_rx = 0;
        const uint32_t want = static_cast<uint32_t>(
            max < hydra::config::AFXDP_RX_BATCH_SIZE
                ? max
                : hydra::config::AFXDP_RX_BATCH_SIZE);
        const uint32_t rcvd = xsk_ring_cons__peek(&rx_ring_, want, &idx_rx);
        if (rcvd == 0)
        {
            return 0;
        }

        for (uint32_t i = 0; i < rcvd; ++i)
        {
            const xdp_desc *desc = xsk_ring_cons__rx_desc(&rx_ring_, idx_rx++);
            out[i].addr = desc->addr;
            out[i].len = desc->len;
        }
        xsk_ring_cons__release(&rx_ring_, rcvd);
        return rcvd;
    }

    uint8_t *XdpSocket::frame_data(uint64_t addr) noexcept
    {
        return static_cast<uint8_t *>(xsk_umem__get_data(umem_area_, addr));
    }

    void XdpSocket::release_frame(uint64_t addr) noexcept
    {
        if (free_frame_count_ < free_frames_.size())
        {
            free_frames_[free_frame_count_++] = addr;
        }
        // free_frame_count_ can never legitimately exceed the UMEM's frame
        // count; if it would, the caller double-released a frame -- drop
        // it rather than corrupt the allocator.
    }

    int XdpSocket::fd() const noexcept
    {
        return xsk_socket__fd(socket_);
    }

    void XdpSocket::refill_fill_ring() noexcept
    {
        if (free_frame_count_ == 0)
        {
            return;
        }
        const uint32_t want = static_cast<uint32_t>(
            xsk_prod_nb_free(&fill_ring_, static_cast<uint32_t>(free_frame_count_)));
        const uint32_t stock = (want < free_frame_count_)
                                    ? want
                                    : static_cast<uint32_t>(free_frame_count_);
        if (stock == 0)
        {
            return;
        }

        uint32_t idx_fq = 0;
        const uint32_t reserved = xsk_ring_prod__reserve(&fill_ring_, stock, &idx_fq);
        for (uint32_t i = 0; i < reserved; ++i)
        {
            *xsk_ring_prod__fill_addr(&fill_ring_, idx_fq++) =
                free_frames_[--free_frame_count_];
        }
        xsk_ring_prod__submit(&fill_ring_, reserved);
    }

    xdp_program *load_and_attach_xdp_program(const std::string &ifname,
                                              const std::string &bpf_object_path,
                                              const std::string &section_name,
                                              xdp_attach_mode mode)
    {
        xdp_program *prog =
            xdp_program__open_file(bpf_object_path.c_str(), section_name.c_str(), nullptr);
        long err = libxdp_get_error(prog);
        if (err)
        {
            char errmsg[256];
            libxdp_strerror(static_cast<int>(err), errmsg, sizeof(errmsg));
            throw std::runtime_error("XdpSocket: failed to open XDP program '" +
                                      bpf_object_path + "': " + errmsg);
        }

        const int ifindex = static_cast<int>(if_nametoindex(ifname.c_str()));
        if (ifindex == 0)
        {
            xdp_program__close(prog);
            throw std::runtime_error("XdpSocket: unknown interface '" + ifname + "'");
        }

        err = xdp_program__attach(prog, ifindex, mode, 0);
        if (err)
        {
            char errmsg[256];
            libxdp_strerror(static_cast<int>(err), errmsg, sizeof(errmsg));
            xdp_program__close(prog);
            throw std::runtime_error("XdpSocket: failed to attach XDP program to '" +
                                      ifname + "': " + errmsg);
        }

        return prog;
    }

    int find_xsks_map_fd(xdp_program *prog)
    {
        bpf_object *obj = xdp_program__bpf_obj(prog);
        bpf_map *map = bpf_object__find_map_by_name(obj, "xsks_map");
        if (map == nullptr)
        {
            throw std::runtime_error(
                "XdpSocket: loaded XDP program has no 'xsks_map' map "
                "(wrong .o loaded?)");
        }
        const int fd = bpf_map__fd(map);
        if (fd < 0)
        {
            throw std::runtime_error("XdpSocket: 'xsks_map' has an invalid fd");
        }
        return fd;
    }

    void detach_xdp_program(xdp_program *prog, const std::string &ifname,
                             xdp_attach_mode mode) noexcept
    {
        if (prog == nullptr)
        {
            return;
        }
        const int ifindex = static_cast<int>(if_nametoindex(ifname.c_str()));
        if (ifindex != 0)
        {
            xdp_program__detach(prog, ifindex, mode, 0);
        }
        xdp_program__close(prog);
    }

} // namespace hydra::xdp
