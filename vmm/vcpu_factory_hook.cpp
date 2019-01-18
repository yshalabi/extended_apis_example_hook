//
// Copyright (C) 2019 Assured Information Security, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <bfcallonce.h>

#include <bfvmm/vcpu/vcpu_factory.h>
#include <eapis/hve/arch/intel_x64/vcpu.h>

using namespace eapis::intel_x64;

// -----------------------------------------------------------------------------
// vCPU
// -----------------------------------------------------------------------------

namespace test
{

bfn::once_flag flag{};

ept::mmap::entry_type g_guest_pte_shadow{};

// vCPU Subclass
//
// All VMM extensions start with subclassing the vCPU and then provide a
// vCPU factory that creates your vCPU when a vCPU is needed. The APIs
// that are provided by the hypervisor and it's extensions are all accessible
// from the vCPU itself.
//
// Since we would like to inherit APIs from the EAPIs extension, we subclass
// the EAPIs version of the vCPU.
//
class vcpu : public eapis::intel_x64::vcpu
{
    // The following stores the:
    // - Guest virtual address of the hello_world() function
    // - Guest physical address of the hello_world() function
    // - Guest virtual address of the hooked_hello_world() function
    //
    uintptr_t m_hello_world_gva{};
    uintptr_t m_hello_world_gpa{};
    uintptr_t m_hooked_hello_world_gva{};

    // The following stores the page table entry (PTE) that represents the
    // guest physical address of the hello_world() function. We will flip
    // the "execute access" bit in this PTE to control access to the
    // hello_world() function
    //
    // Note that we use a reference_wrapper instead of a pointer to prevent
    // the possibility of accidentally dereferencing a nullptr.
    //
    std::reference_wrapper<ept::mmap::entry_type> m_pte{g_guest_pte_shadow};
    ept::mmap vcpu_guest_map{};

public:

    // Constructor
    //
    // This is the only constructor the vCPU supports, so it must be
    // overloaded.
    //
    vcpu(vcpuid::type id) :
        eapis::intel_x64::vcpu{id}
    {
        using namespace vmcs_n;
        using mt_delegate_t = monitor_trap_handler::handler_delegate_t;
        using eptv_delegate_t = ept_violation_handler::handler_delegate_t;

        // Add a VMCall handler. This will catch the VMCalls made by the
        // userspace application and call the vmcall_handler() function.
        //
        this->add_handler(
            exit_reason::basic_exit_reason::vmcall,
            ::handler_delegate_t::create<vcpu, &vcpu::vmcall_handler>(this)
        );

        // Add a Monitor Trap handler. This will catch Monitor Trap VM exits
        // and call the monitor_trap_handler() function. We will use the
        // monitor trap flag to single step attempts to execute code that
        // exists in the same physical page as our hello_world() function.
        //
        this->add_monitor_trap_handler(
            mt_delegate_t::create<vcpu, &vcpu::monitor_trap_handler>(this)
        );

        // Add an EPT violation handler (for execute access). If an EPT
        // violation is made for execute accesses,  ept_execute_violation_handler()
        // will be called which is where we will perform our hook.
        //
        this->add_ept_execute_violation_handler(
            eptv_delegate_t::create<vcpu, &vcpu::ept_execute_violation_handler>(this)
        );

        // Setup EPT. This will create our EPT memory map for the host OS. Note
        // that we use the identity_map() function as this ensures the MTRRs
        // are respected, and we map memory up to MAX_PHYS_ADDR which can be
        // changed if you system has a ton of extra physical memory. You could
        // also use the max physical memory returned by CPUID.
        //
        // Also note that we only call this function once as the EPT map in
        // this example is a global resource so it only needs to be set up
        // once and then can be used by the remaining cores.
        //
        //bfn::call_once(flag, [&] {
            ept::identity_map(
                vcpu_guest_map,
                MAX_PHYS_ADDR
            );
        //});
    }

    bool
    vmcall_handler(
        gsl::not_null<vcpu_t *> vcpu)
    {
        // If a VMCall is made, we either need to install our hook, or we
        // need to turn it off (uninstall it).
        //
        // Note that we use guard_exceptions() which will prevent an exception
        // from crashing the hypervisor. Instead, the exception will be
        // sent to the serial device, and the vmcall will return safely.
        //
        guard_exceptions([&] {
            switch(this->rax()) {
                case 0:
                    this->vmcall_handler_hook(vcpu);
                    break;

                default:
                    this->vmcall_handler_unhook(vcpu);
                    break;
            };
        });

        // Make sure we advance the instruction pointer. Otherwise, the VMCall
        // instruction will be executed in an infinite look. Also note that
        // the advance() function always returns true, which tells the base
        // hypervisor that this VM exit was successfully handled.
        //
        return this->advance();
    }

    void
    vmcall_handler_hook(
        gsl::not_null<vcpu_t *> vcpu)
    {
        bfignored(vcpu);
        // Store the guest virtual address of both the hello_world() function
        // and the hooked_hello_world() function
        //
        m_hello_world_gva = this->rbx();
        m_hooked_hello_world_gva = this->rcx();

        // The virtual address of the hello_world() function is a guest virtual
        // address. We need to use the guest's CR3 to figure out what the
        // guest's physical address of this virtual address is. The following
        // performs this conversion by parsing the guest's pages tables to
        // get the guest physical address, and since either EPT is not used
        // or it is a 1:1 map, the guest physical address == the host physical
        // address all the time so we can use the gpa as an hpa as needed.
        //
        auto [hpa, ignored1] = this->gva_to_hpa(m_hello_world_gva);

        m_hello_world_gpa = hpa;
        bfignored(ignored1);

        // Now that we know what the physical address of the hello_world()
        // function is, we need to get the EPT PTE associated with this physical
        // address. The problem is, EPT was set up using 2M pages, which is
        // large. On x86_64, this would basically cause us to trap on every
        // single memory access of the entire userspace application (as
        // applications in 64bit are setup with 2M pages, typically). The
        // following converts our 2M page into 4K pages so that we can get
        // the PTE of just the 4k page that has our hello_world() application.
        //
        ept::identity_map_convert_2m_to_4k(
            vcpu_guest_map,
            bfn::upper(m_hello_world_gpa, ::intel_x64::ept::pd::from)
        );

        // Get the 4k PTE associated with our hello_world() application
        //
        auto [pte, ignored2] = vcpu_guest_map.entry(m_hello_world_gpa);

        m_pte = pte;
        bfignored(ignored2);

        // Disable execute access for the page associated with our
        // hello_world() application. Any attempt to
        // execute code on this page will generate an EPT violation which
        // will present us with an opportunity to hook the hello_world()
        // function
        //
        ::intel_x64::ept::pt::entry::execute_access::disable(m_pte);

        // Tell the VMCS to use our new EPT map
        //
        this->set_eptp(vcpu_guest_map);
    }

    bool ept_execute_violation_handler(
        gsl::not_null<vcpu_t *> vcpu, ept_violation_handler::info_t &info)
    {
        bfignored(vcpu);
        bfignored(info);

        // If we got an EPT violation (i.e. this function was executed), it
        // means that our userspace application attempted to execute code in
        // the page that has our hello_world() function. 4k bytes worth of code
        // is sitting in this page, so we first need to check if the execute
        // access was actually the hello_world() function, or something else.
        // If this was our hello_world() function, we need to change the
        // guest's instruction pointer towards our hooked_hello_world() function
        // instead
        //
        if (this->rip() == m_hello_world_gva) {
            this->set_rip(m_hooked_hello_world_gva);
        }

        // Before we finish, we need to reenable execute access, otherwise
        // when this function finishes, an EPT violation will occur again.
        // The problem is, once we enable access to this page, we will stop
        // generating EPT violations, which will prevent us from installing
        // our hook if needed. To solve this, we single step the memory access
        // so that once it is done executing, we can disable execute access to
        // the page again. We do this by turning on the monitor trap flag.
        //
        this->enable_monitor_trap_flag();
        ::intel_x64::ept::pt::entry::execute_access::enable(m_pte);

        // Return true, telling the base hypervisor that we have handled the
        // VM exit. Note that since this is an EPT violation, we do not
        // flush the TLB as the hardware will do this for us.
        //
        return true;
    }

    bool monitor_trap_handler(
        gsl::not_null<vcpu_t *> vcpu, monitor_trap_handler::info_t &info)
    {
        bfignored(vcpu);
        bfignored(info);

        // If this function is executed, it means that our memory access has
        // successfully executed, and we need to disable access to our page
        // so that we can continue to trap on execute accesses to it.
        //
        ::intel_x64::ept::pt::entry::execute_access::disable(m_pte);
        ::intel_x64::vmx::invept_global();

        // Return true, telling the base hypervisor that we have handled the
        // VM exit.
        return true;
    }

    void
    vmcall_handler_unhook(
        gsl::not_null<vcpu_t *> vcpu)
    {
        bfignored(vcpu);
        m_pte = g_guest_pte_shadow;

        // To uninstall our hook, we need to convert our 4k pages back to a
        // single 2M page. This will ensure that the next time our userspace
        // application is executed, we can repeat our hook process over, and
        // over, and over without our EPT map getting distorted over time.
        //
        ept::identity_map_convert_4k_to_2m(
            vcpu_guest_map,
            bfn::upper(m_hello_world_gpa, ::intel_x64::ept::pd::from)
        );

        // Clear our saved addresses as they are no longer valid.
        //
        m_hello_world_gva = {};
        m_hello_world_gpa = {};
        m_hooked_hello_world_gva = {};

        this->disable_ept();
    }
};

}

// -----------------------------------------------------------------------------
// vCPU Factory
// -----------------------------------------------------------------------------

namespace bfvmm
{

// vCPU Factory
//
// This function creates vCPUs when they are needed. This is required by all
// extensions. When the vCPU manager is told to create a vCPU, it calls this
// function, which you use in your extenion to create your customer vCPU
// which has all of your custom VMM logic in it.
//
std::unique_ptr<vcpu>
vcpu_factory::make(vcpuid::type vcpuid, bfobject *obj)
{
    bfignored(obj);
    return std::make_unique<test::vcpu>(vcpuid);
}

}
