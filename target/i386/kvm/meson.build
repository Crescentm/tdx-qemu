i386_ss.add(when: 'CONFIG_KVM', if_false: files('kvm-stub.c'))

i386_softmmu_kvm_ss = ss.source_set()

i386_softmmu_kvm_ss.add(files(
  'kvm.c',
  'kvm-cpu.c',
))

i386_softmmu_kvm_ss.add(when: 'CONFIG_SEV', if_false: files('sev-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx.c'), if_false: files('tdx-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx-vtpm.c'), if_false: files('tdx-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx-vtpm-server.c'), if_false: files('tdx-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx-vtpm-client.c'), if_false: files('tdx-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx-service-query.c'), if_false: files('tdx-stub.c'))

i386_softmmu_kvm_ss.add(when: 'CONFIG_TDX', if_true: files('tdx-vmcall-service.c'), if_false: files('tdx-stub.c'))

i386_softmmu_ss.add(when: 'CONFIG_HYPERV', if_true: files('hyperv.c'), if_false: files('hyperv-stub.c'))

i386_softmmu_ss.add_all(when: 'CONFIG_KVM', if_true: i386_softmmu_kvm_ss)
