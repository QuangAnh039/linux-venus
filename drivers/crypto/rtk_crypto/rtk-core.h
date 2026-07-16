#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <crypto/aes.h>
#include <crypto/internal/des.h>
#include <crypto/engine.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/skcipher.h>
#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>
#include <crypto/internal/hash.h>
#include <crypto/scatterwalk.h>
#include <crypto/authenc.h>
#include <crypto/gcm.h>
#include <crypto/null.h>

#include "rtk-base-ctrl.h"
#include "rtk-base-type.h"
#include "rtk-reg.h"

#define ERR_LOG  1
#define WARN_LOG 1
#define INFO_LOG 0

#undef DBG_CRYPTO_ERR
#define DBG_CRYPTO_ERR(fmt, args...) \
	     if (ERR_LOG) { \
	     	printk("%s(): " fmt " ", "rtl_crypto_err", ##args); \
	     }

#undef DBG_CRYPTO_WARN
#define DBG_CRYPTO_WARN(fmt, args...) \
	     if (WARN_LOG) { \
	     	printk("%s(): " fmt " ", "rtl_crypto_warn", ##args); \
	     }

#undef DBG_CRYPTO_INFO
#define DBG_CRYPTO_INFO(fmt, args...) \
	     if (INFO_LOG) { \
	     	printk("%s(): " fmt " ", "rtl_crypto_info", ##args); \
	     }

//*******************
// CIPHER definition
//*******************

//bit 0,1,2,3 for cipher alg
#define CIPHER_TYPE_MASK_BLOCK      0xF
#define FLG_ECB       0x0
#define FLG_CBC       0x1
#define FLG_CFB       0x2
#define FLG_OFB       0x3
#define FLG_CTR       0x4
#define FLG_GCTR      0x5
#define FLG_GMAC      0x6
#define FLG_GHASH     0x7
#define FLG_GCM       0x8

//bit 4 for DECRYPT/ENCRYPT
#define CIPHER_CRYPT_MASK	0x10
#define FLG_DECRYPT	0x00
#define FLG_ENCRYPT	0x10

//bit 5,6 for AES/DES/TDES
#define CIPHER_KEY_MODE_MASK     0x60
#define FLG_AES		0x20
#define FLG_DES		0x40
#define FLG_TDES	0x60

//bit 7,8 for aead hash flag
#define AEAD_HASH_TYPE_MASK_BLOCK      0x180
#define FLG_AEAD_MD5         0x000
#define FLG_AEAD_SHA1        0x080
#define FLG_AEAD_SHA2_224    0x100
#define FLG_AEAD_SHA2_256    0x180

//*******************
// HASH definition
//*******************

//bit 0,1 for hash alg
#define HASH_TYPE_MASK_BLOCK      0x3
#define FLG_MD5         0x0
#define FLG_SHA1        0x1
#define FLG_SHA2_224    0x2
#define FLG_SHA2_256    0x3

//bit 2 for HMAC mode
#define FLG_HMAC        0x4

//bit 3, 4 for update, final opreation
#define HASH_OP_MASK	0x18
#define FLG_UPDATE	0x8
#define FLG_FINAL	0x10

//*******************
// feature definition
//*******************

//bit 7,8 for hash/cipher/mix mode
#define CIPHER_HASH_TYPE_MASK	0x180
#define FLG_HASH	0x080
#define FLG_CIPHER	0x100
#define FLG_MIX		0x180


#define MAX_CRYPTO_DST_LEN	16777215
#define MAX_CRYPTO_SRC_LEN	16383

//if set TASKLET_MODE to 0, it will use irq_thread to handle bottom half
#define TASKLET_MODE 1

#define MAX_HASH_MSG_SIZE 16320

struct sdesc {
	struct shash_desc shash;
	char ctx[];
};

struct rtk_cryp {
	struct crypto_engine *engine;
	void __iomem *base_addr;
	struct device *dev;
	struct skcipher_request *cipher_req;
	struct ahash_request *hash_req;
	struct aead_request *aead_req;
#if TASKLET_MODE
	struct tasklet_struct done_task;
#endif
	int req_err;
};

struct rtk_cryp_tfm_ctx {
	struct rtk_cryp *cryp; 
	u8 *key;
	u32 keylen;
	dma_addr_t p_key;
	u8 *iv;
	u32 ivlen;
	dma_addr_t p_iv;
	u8 *cmd_setting;
	dma_addr_t p_cmd_setting;
	u8 *cryp_padding;
	u32 cryp_padding_len;
	dma_addr_t p_cryp_padding;
	u32 nr_sgs;
	u32 nr_sgd;
};

struct rtk_cryp_reqctx {
	unsigned long mode;
};

struct rtk_hash_tfm_ctx {
	struct rtk_cryp *cryp;
	u8 *src_buf;
	dma_addr_t p_src_buf;
	u32 total_len;
	u32 last_remain_len;
	u8 *digest_result;
	dma_addr_t p_digest_result;
	u8 *cmd_setting;
	dma_addr_t p_cmd_setting;
	u8 *hash_padding;
	u32 hash_padding_len;
	dma_addr_t p_hash_padding;
	u8 *keypad;
	u32 keypad_len;
	dma_addr_t p_keypad;
	u32 dma_unmap_flag;
	u32 restlen;
	u8 tmp_src_buf[64];
};

struct rtk_hash_reqctx {
	unsigned long mode;
};

struct rtk_aead_tfm_ctx {
	struct rtk_cryp *cryp;
	u8 *key;
	u32 keylen;
	dma_addr_t p_key;
	u8 *iv;
	u32 ivlen;
	dma_addr_t p_iv;
	u8 *cmd_setting;
	dma_addr_t p_cmd_setting;
	u8 *cryp_padding;
	u32 cryp_padding_len;
	dma_addr_t p_cryp_padding;
	u32 cipher_nr_sgs;
	u32 cipher_nr_sgd;
	u32 aad_nr_sgs;
	u32 total_len;
	u8 *digest_result;
	dma_addr_t p_digest_result;
	u32 hash_padding_len;
	u8 *keypad;
	u32 keypad_len;
	dma_addr_t p_keypad;
	struct crypto_sync_skcipher *null;
	struct crypto_aes_ctx actx;
	u32 a2eo_padding_len;
};

struct rtk_aead_reqctx {
	unsigned long mode;
	struct scatterlist cipher_src[2];
	struct scatterlist cipher_dst[2];
	u32 first_phase;
	struct crypto_wait *wait;
	struct scatterlist *crypto_src;
	struct scatterlist *crypto_dst;
};

extern struct rtk_cryp *crypto;

int rtk_gcm_alg_register(void);
int rtk_aead_alg_register(void);
int rtk_hash_alg_register(void);
int rtk_crypto_alg_register(void);
int rtk_cryp_cipher_one_req(struct crypto_engine *engine, void *areq);
int rtk_cryp_unprepare_cipher_req(struct crypto_engine *engine, void *areq);
int rtk_cryp_prepare_cipher_req(struct crypto_engine *engine, void *areq);
int set_dst_desc(CRYPTO_Type *base, u32 value, dma_addr_t addr);
int set_src_desc(CRYPTO_Type *base, u32 value, dma_addr_t addr);
void hexdump(unsigned char *buf, unsigned int len);
