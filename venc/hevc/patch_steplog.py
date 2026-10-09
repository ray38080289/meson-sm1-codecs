"""Step-by-step power-up logging (pr_emerg, so netconsole flushes each line)."""
def rep(s, old, new):
    assert s.count(old) == 1, (s.count(old), old[:70])
    return s.replace(old, new, 1)

p = 'vpu.c'
s = open(p).read()

s = rep(s, '''		if (get_cpu_type() >= MESON_CPU_MAJOR_ID_SC2) {

		} else
		    amports_switch_gate("vdec", 1);

		spin_lock_irqsave(&s_vpu_lock, flags);
''', '''		HENC_STEP("open: before dos gate");
		if (get_cpu_type() >= MESON_CPU_MAJOR_ID_SC2) {

		} else
		    amports_switch_gate("vdec", 1);
		HENC_STEP("open: dos gate on");

		spin_lock_irqsave(&s_vpu_lock, flags);
''')
s = rep(s, '''		udelay(10);

		if (get_cpu_type() <= MESON_CPU_MAJOR_ID_TXLX) {''', '''		udelay(10);
		HENC_STEP("open: sleep0 cleared");

		if (get_cpu_type() <= MESON_CPU_MAJOR_ID_TXLX) {''')
s = rep(s, '''#ifndef VPU_SUPPORT_CLOCK_CONTROL
		vpu_clk_config(1);
#endif
		/* Enable wave420l_vpu_idle_rise_irq,''', '''		HENC_STEP("open: resets pulsed");
#ifndef VPU_SUPPORT_CLOCK_CONTROL
		vpu_clk_config(1);
#endif
		HENC_STEP("open: clocks on");
		/* Enable wave420l_vpu_idle_rise_irq,''')
s = rep(s, '''				? ~0x8 : ~(0x3<<12)));
		}
		spin_unlock_irqrestore(&s_vpu_lock, flags);
	}''', '''				? ~0x8 : ~(0x3<<12)));
		}
		HENC_STEP("open: iso0 cleared");
		pr_emerg("HevcEnc: product number %#x (expect 0x4201)\\n",
			 readl((void __iomem *)s_vpu_register.virt_addr + 0x1044));
		spin_unlock_irqrestore(&s_vpu_lock, flags);
	}''')
# helper
s = rep(s, '#include "compat.h"\n', '''#include "compat.h"

#define HENC_STEP(what) \\
	pr_emerg("HevcEnc: %s: sleep0=%#x iso0=%#x clk=%#x clk2=%#x memPd=%#x\\n", \\
		 what, READ_AOREG(AO_RTI_GEN_PWR_SLEEP0), \\
		 READ_AOREG(AO_RTI_GEN_PWR_ISO0), \\
		 READ_HHI_REG(HHI_WAVE420L_CLK_CNTL), \\
		 READ_HHI_REG(HHI_WAVE420L_CLK_CNTL2), \\
		 READ_VREG(DOS_MEM_PD_WAVE420L))
''')
open(p, 'w').write(s)
print('patched')
