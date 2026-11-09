#ifndef __LOONGGPU_MMU_H__
#define __LOONGGPU_MMU_H__

#define LOONGGPU_MMU_GEN_CTRL_OFFSET       0x10000
#define LOONGGPU_MMU_EXC_CTRL_OFFSET       0x10004
#define LOONGGPU_MMU_EXADDR_LO_OFFSET      0x10008
#define LOONGGPU_MMU_EXADDR_HI_OFFSET      0x1000c
#define LOONGGPU_MMU_SAFE_LO_OFFSET        0x10010
#define LOONGGPU_MMU_SAFE_HI_OFFSET        0x10014
#define LOONGGPU_MMU_FLUSH_CTRL_OFFSET     0x10018
#define LOONGGPU_MMU_MISC_CTRL_OFFSET      0x1001c
#define LOONGGPU_MMU_PGD_LO_OFFSET         0x10020
#define LOONGGPU_MMU_PGD_HI_OFFSET         0x10024
#define LOONGGPU_MMU_DIR_CTRL_OFFSET       0x100a0


#define MMU_ENABLE 						0x01
#define MMU_SET_PGD						0x02
#define MMU_SET_SAFE 					0x03
#define MMU_SET_DIR						0x04
#define MMU_SET_EXC						0x05
#define MMU_FLUSH						0x06


#define LOONGGPU_MMU_PTE_SIZE 				8
#define LOONGGPU_MMU_PGD_REG_SIZE    		8
#define LOONGGPU_MMU_VMID_OF_PGD(vmid)   \
			     (LOONGGPU_MMU_PGD_LO_OFFSET + ((vmid) * LOONGGPU_MMU_PGD_REG_SIZE))

#if (LOONGGPU_GPU_PAGE_SIZE == SZ_4K)
#define LOONGGPU_MMU_DIR_CTRL_256M_1LVL  ((14 << 26 | 0 << 20) | \
						(14 << 16 | 0 << 10) | \
						(15 <<  6 | 12 <<  0))

#define LOONGGPU_MMU_DIR_CTRL_512M_1LVL  LOONGGPU_MMU_DIR_CTRL_256M_1LVL

#define LOONGGPU_MMU_DIR_CTRL_1T_3LVL  ((10 << 26 | 30 << 20) | \
						(9 << 16 | 21 << 10) | \
						(9 <<  6 | 12 <<  0))
#endif

#if (LOONGGPU_GPU_PAGE_SIZE == SZ_16K)
#define LOONGGPU_MMU_DIR_CTRL_256M_1LVL  ((14 << 26 | 0 << 20) | \
						(14 << 16 | 0 << 10) | \
						(14 <<  6 | 14 <<  0))

#define LOONGGPU_MMU_DIR_CTRL_512M_1LVL  ((14 << 26 | 0 << 20) | \
						(14 << 16 | 0 << 10) | \
						(15 <<  6 | 14 <<  0))

#define LOONGGPU_MMU_DIR_CTRL_1T_3LVL  ((4 << 26 | 36 << 20) | \
						(11 << 16 | 25 << 10) | \
						(11 <<  6 | 14 <<  0))
#endif

#define LOONGGPU_MMU_FLUSH_DOMAIN_SHIFT 	8
#define LOONGGPU_MMU_FLUSH_EN 				BIT(0)
#define LOONGGPU_MMU_FLUSH_ALL 			BIT(1)
#define LOONGGPU_MMU_FLUSH_VMID 			0

#define LOONGGPU_MMU_FLUSH_PKT(vmid, all) \
			     ((vmid << LOONGGPU_MMU_FLUSH_DOMAIN_SHIFT) | (all) | LOONGGPU_MMU_FLUSH_EN)

#define LOONGGPU_LG2XX_MMU_EN_FAULT	0x7fff0000

extern const struct loonggpu_ip_block_version mmu_ip_block;

#endif /*__LOONGGPU_MMU_H__*/
