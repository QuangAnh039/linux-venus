#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include "rtk-core.h"

struct rtk_cryp *crypto;

void hexdump(unsigned char *buf, unsigned int len)
{
	print_hex_dump(KERN_CONT, "", DUMP_PREFIX_OFFSET,
			16, 1,
			buf, len, false);
}

int rtk_cryp_prepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = container_of(areq, struct skcipher_request, base);
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(req);
	struct rtk_cryp_reqctx *rctx = skcipher_request_ctx(req);
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(skcipher);
	struct rtk_cryp *cryp = ctx->cryp; // need to check?
	CRYPTO_Type *base = NULL;
	rtl_crypto_cl_t *cmd_ptr;
	int nr_sgs;
	int nr_sgd;

	if (!cryp) {
		DBG_CRYPTO_ERR("Prepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	base = cryp->base_addr;

	//clear command ok & error interrupts	
	base->ipscsr_err_stats_reg = 0xFFFF;
	base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;

	//store req in cryp
	cryp->cipher_req = req;
	cryp->hash_req = NULL;
	cryp->aead_req = NULL;

	//prepare command setting buffer
	cmd_ptr = (rtl_crypto_cl_t *)ctx->cmd_setting;
	memset((u8*)cmd_ptr, 0, 32);

	//encrypt/decrypt
	if (rctx->mode & FLG_ENCRYPT)
		cmd_ptr->cipher_encrypt = 1;

	//AES/DES/TDES
	switch (rctx->mode & CIPHER_KEY_MODE_MASK)
	{
		case FLG_AES:
			cmd_ptr->cipher_eng_sel = 0;
			cmd_ptr->cabs = 1; //??
			
			switch (ctx->keylen)
			{
				case AES_KEYSIZE_128:
					cmd_ptr->aes_key_sel = 0;
					break;
				case AES_KEYSIZE_192:
					cmd_ptr->aes_key_sel = 1;
					break;
				case AES_KEYSIZE_256:
					cmd_ptr->aes_key_sel = 2;
					break;
				default:
					DBG_CRYPTO_ERR("Wrong Keylen\n");
					return -EINVAL;
			}

			cmd_ptr->cipher_mode = rctx->mode & CIPHER_TYPE_MASK_BLOCK;

			//AES take 16bytes as a block to process data
			cmd_ptr->enl = (req->cryptlen + 15)/AES_BLOCK_SIZE;
			cmd_ptr->enc_last_data_size = req->cryptlen % AES_BLOCK_SIZE;
			ctx->cryp_padding_len = (AES_BLOCK_SIZE - (req->cryptlen % AES_BLOCK_SIZE)) % AES_BLOCK_SIZE;
			
			break;
		case FLG_TDES:
			cmd_ptr->des3_en = 1;
			fallthrough;
		case FLG_DES:
			cmd_ptr->cipher_eng_sel = 1;
			//DES take 8bytes as a block to process data
			cmd_ptr->enl = (req->cryptlen + 7)/DES_BLOCK_SIZE;
			cmd_ptr->enc_last_data_size = req->cryptlen % DES_BLOCK_SIZE;
			ctx->cryp_padding_len = (DES_BLOCK_SIZE - (req->cryptlen % DES_BLOCK_SIZE)) % DES_BLOCK_SIZE;
			cmd_ptr->cipher_mode = rctx->mode & CIPHER_TYPE_MASK_BLOCK;
			break;
		default:
			DBG_CRYPTO_ERR("Prepare cipher req: ALG_MODE unknown, it must be AES/DES/TDES\n");
	}

	cmd_ptr->icv_total_length = 0x40; // for mix mode, but need to set a value 0x40

	wmb();

	if (!req->src || !req->dst) {
		DBG_CRYPTO_ERR("Prepare cipher req: src or dst is NULL\n");
		return -EINVAL;
	}

	//dma_map scatterlist
	if (req->src == req->dst) {
		nr_sgs = dma_map_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->cryptlen), DMA_BIDIRECTIONAL);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid src/dst sg number %d\n", nr_sgs);
			return -EINVAL;
		}
		nr_sgd = nr_sgs;

		ctx->nr_sgs = nr_sgs;
		ctx->nr_sgd = nr_sgd;
	}
	else {
		nr_sgs = dma_map_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->cryptlen), DMA_TO_DEVICE);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid src sg number %d\n", nr_sgs);
			return -EINVAL;
		}
		ctx->nr_sgs = nr_sgs;
		nr_sgd = dma_map_sg(cryp->dev, req->dst, sg_nents_for_len(req->dst, req->cryptlen), DMA_FROM_DEVICE);
		if (nr_sgd <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid dst sg number %d\n", nr_sgd);
			dma_unmap_sg(cryp->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
			return -EINVAL;
		}
		ctx->nr_sgd = nr_sgd;
	}


	//prepare iv array
	if ((rctx->mode & CIPHER_TYPE_MASK_BLOCK) != FLG_ECB) {
		if (req->iv) {
			ctx->ivlen = crypto_skcipher_ivsize(skcipher);
			memcpy(ctx->iv, req->iv, ctx->ivlen);
			if (rctx->mode & FLG_CBC) {
				if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
					sg_pcopy_to_buffer(cryp->cipher_req->src, ctx->nr_sgs,
						cryp->cipher_req->iv, crypto_skcipher_ivsize(skcipher),
						cryp->cipher_req->cryptlen - crypto_skcipher_ivsize(skcipher));
				}
			}
		}
	}

	return 0;
}

int rtk_cryp_unprepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = container_of(areq, struct skcipher_request, base);
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(req);
	struct rtk_cryp_reqctx *rctx = skcipher_request_ctx(req);
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(skcipher);
	struct rtk_cryp *cryp = ctx->cryp;
	int i, round, block_size;

	if (!cryp) {
		DBG_CRYPTO_ERR("Unprepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	//unmap dma
	if (req->src == req->dst)
		//dma_unmap_sg(cryp->dev, req->src, sg_nents(req->src), DMA_BIDIRECTIONAL);
		dma_unmap_sg(cryp->dev, req->src, ctx->nr_sgs, DMA_BIDIRECTIONAL);
	else {
		//dma_unmap_sg(cryp->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
		dma_unmap_sg(cryp->dev, req->src,ctx->nr_sgs, DMA_TO_DEVICE);
		//dma_unmap_sg(cryp->dev, req->dst, sg_nents(req->dst), DMA_FROM_DEVICE);
		dma_unmap_sg(cryp->dev, req->dst, ctx->nr_sgd, DMA_FROM_DEVICE);
	}

	if (rctx->mode & FLG_CBC) {
		if (rctx->mode & FLG_ENCRYPT) {
			sg_pcopy_to_buffer(cryp->cipher_req->dst, ctx->nr_sgd,
				cryp->cipher_req->iv, crypto_skcipher_ivsize(skcipher),
				cryp->cipher_req->cryptlen - crypto_skcipher_ivsize(skcipher));
		}
	}
	else if (rctx->mode & FLG_CTR) {
		switch (rctx->mode & CIPHER_KEY_MODE_MASK)
		{
			case FLG_AES:
				block_size = AES_BLOCK_SIZE;
				break;
			case FLG_DES:
				block_size = DES_BLOCK_SIZE;
				break;
			case FLG_TDES:
				block_size = DES3_EDE_BLOCK_SIZE;
				break;
		}

		round = DIV_ROUND_UP(req->cryptlen, block_size);
		for (i = 0; i < round; i++) {
			crypto_inc(req->iv, block_size);
		}
	}

	return 0;
}

int set_src_desc(CRYPTO_Type *base, u32 value, dma_addr_t addr)
{
	if (base->srcdesc_status_reg_b.fifo_empty_cnt > 0) {
		writel(value, &base->sdfw_reg);
		writel(addr, &base->sdsw_reg);
	}
	else {
		DBG_CRYPTO_ERR("Set src desc: fifo is full\n");
		return -ENOMEM;
	}
	return 0;
}

int set_dst_desc(CRYPTO_Type *base, u32 value, dma_addr_t addr)
{
	if (base->dstdesc_status_reg_b.fifo_empty_cnt > 0) {
		writel(value, &base->ddfw_reg);
		writel(addr, &base->ddsw_reg);
	}
	else {
		DBG_CRYPTO_ERR("Set dst desc: fifo is full\n");
		return -ENOMEM;
	}
	return 0;
}

static int rtk_hal_crypto_set_srcdesc(struct skcipher_request *req, struct rtk_cryp_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_srcdesc_t  src_cipher_desc;
	struct scatterlist *sg;
	struct scatterlist *src_sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen = req->cryptlen;

	//todo: additional authentication data buffer (aad)

	src_sg = req->src;
	for_each_sg(src_sg, sg, ctx->nr_sgs, i) {
		src_cipher_desc.w = 0;
		src_cipher_desc.d.rs = 1;

		len = sg_dma_len(sg);
		addr = sg_dma_address(sg);

		// 16353 bytes is the max size of src enl
		while (len > MAX_CRYPTO_SRC_LEN) {
			if (cryptlen <= MAX_CRYPTO_SRC_LEN) {
				src_cipher_desc.d.enl = cryptlen;

				if (ctx->cryp_padding_len == 0) {
					src_cipher_desc.d.ls = 1;
				}

				ret = set_src_desc(base, src_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_src_done;
			}
			else {
				src_cipher_desc.d.enl = MAX_CRYPTO_SRC_LEN;
			}

			ret = set_src_desc(base, src_cipher_desc.w, addr);
			if (ret)
				return ret;

			addr = addr + MAX_CRYPTO_SRC_LEN;
			len = len - MAX_CRYPTO_SRC_LEN;
			cryptlen = cryptlen - MAX_CRYPTO_SRC_LEN;

			src_cipher_desc.w = 0;
			src_cipher_desc.d.rs = 1;
		}

		if (len) {
			if (cryptlen <= len) {
				src_cipher_desc.d.enl = cryptlen;

				if (ctx->cryp_padding_len == 0) {
					src_cipher_desc.d.ls = 1;
				}

				ret = set_src_desc(base, src_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_src_done;
			}
			else {
				src_cipher_desc.d.enl = len;
			}

			cryptlen = cryptlen - len;

			if (sg_is_last(sg) && ctx->cryp_padding_len == 0) {
				src_cipher_desc.d.ls = 1;
			}

			ret = set_src_desc(base, src_cipher_desc.w, addr);
			if (ret)
				return ret;
		}
		
	}

	//todo:mix mode not support auto-padding, need to setup Authentication padding

set_src_done:

	//Padding buffer of crypto message
	if (ctx->cryp_padding_len) {
		src_cipher_desc.w = 0;
		src_cipher_desc.d.rs = 1;
		src_cipher_desc.d.enl = ctx->cryp_padding_len;
		src_cipher_desc.d.ls = 1;

		ret = set_src_desc(base, src_cipher_desc.w, ctx->p_cryp_padding);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtk_hal_crypto_set_dstdesc(struct skcipher_request *req, struct rtk_cryp_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_dstdesc_t  dst_cipher_desc;
	struct scatterlist *sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen = req->cryptlen;

	for_each_sg(req->dst, sg, ctx->nr_sgd, i) {
		dst_cipher_desc.w = 0;
		dst_cipher_desc.cipher.ws  = 1;
		dst_cipher_desc.cipher.enc = 1;
		
		if (i == 0) 
			dst_cipher_desc.cipher.fs = 1;

		len = sg_dma_len(sg);
		addr = sg_dma_address(sg);

		// 16777215 bytes is the max size of dst enl
		while (len > MAX_CRYPTO_DST_LEN) {

			//aead test
			if (cryptlen <= MAX_CRYPTO_DST_LEN) {
				dst_cipher_desc.cipher.enl = cryptlen;
				dst_cipher_desc.cipher.ls = 1;

				ret = set_dst_desc(base, dst_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_dst_done;
			}
			else {
				dst_cipher_desc.cipher.enl = MAX_CRYPTO_DST_LEN;
			}

			ret = set_dst_desc(base, dst_cipher_desc.w, addr);
			if (ret)
				return ret;

			addr = addr + MAX_CRYPTO_DST_LEN;
			len = len - MAX_CRYPTO_DST_LEN;
			cryptlen = cryptlen - MAX_CRYPTO_DST_LEN;

			dst_cipher_desc.w = 0;
			dst_cipher_desc.cipher.ws  = 1;
			dst_cipher_desc.cipher.enc = 1;
		}

		if (len) {
			if (cryptlen <= len) {
				dst_cipher_desc.cipher.enl = cryptlen;
				dst_cipher_desc.cipher.ls = 1;

				ret = set_dst_desc(base, dst_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_dst_done;
			}
			else {
				dst_cipher_desc.cipher.enl = len;
			}

			cryptlen = cryptlen - len;

			if (sg_is_last(sg)) {
				dst_cipher_desc.cipher.ls = 1;
				//if (ctx->cryp_padding_len)   // need to check
				//	dst_cipher_desc.cipher.enl += ctx->cryp_padding_len; //need to check
			}
			//printk("len = %d, ctx->cryp_padding_len = %d, dst_cipher_desc.cipher.enl = 0x%x\n", len, ctx->cryp_padding_len, dst_cipher_desc.cipher.enl);

			ret = set_dst_desc(base, dst_cipher_desc.w, addr);
			if (ret)
				return ret;
		}
			

	}

set_dst_done:

	return 0;
}

static int rtk_setup_key_iv_cmd(struct skcipher_request *req, struct rtk_cryp_tfm_ctx *ctx, CRYPTO_Type *base)
{
	struct rtk_cryp_reqctx *rctx = skcipher_request_ctx(req);
	rtl_crypto_srcdesc_t src_desc;
	int ret;

	//set command setting
	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	src_desc.b.cl = 3;

	//todo:hash need set enable auto-padding bit

	ret = set_src_desc(base, src_desc.w, ctx->p_cmd_setting);
	if (ret) {
		DBG_CRYPTO_ERR("Set command setting to src fifo fail\n");
		return ret;
	}

	//set key
	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	//key_len use 4bytes as a unit
	src_desc.b.key_len = (ctx->keylen)/4;

	ret = set_src_desc(base, src_desc.w, ctx->p_key);
	if (ret) {
		DBG_CRYPTO_ERR("Set key to src fifo fail\n");
		return ret;
	}

	//set iv
	if ((rctx->mode & CIPHER_TYPE_MASK_BLOCK) != FLG_ECB) {
		if (req->iv) {
			src_desc.w = 0;
			src_desc.b.rs = 1;
			src_desc.b.fs = 1;

			//iv_len use 4bytes as a unit
			src_desc.b.iv_len = (ctx->ivlen)/4;
			ret = set_src_desc(base, src_desc.w, ctx->p_iv);
			if (ret) {
				DBG_CRYPTO_ERR("Set iv to src fifo fail\n");
				return ret;
			}
		}
	}

	return 0;
} 

int rtk_cryp_cipher_one_req(struct crypto_engine *engine, void *areq)
{
	struct skcipher_request *req = container_of(areq, struct skcipher_request, base);
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(req);
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(skcipher);
	struct rtk_cryp *cryp = ctx->cryp; // need to check?
	CRYPTO_Type *base = NULL;
	int ret = 0;
	int err;

	if (!cryp) {
		DBG_CRYPTO_ERR("Do cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	base = cryp->base_addr;
 
	err = rtk_cryp_prepare_cipher_req(engine, areq);
	if (err)
		goto cipherReqEnd;

	//printk("base->ipscsr_err_stats_reg = 0x%x", base->ipscsr_err_stats_reg);
	// step 1: Setup desitination descriptor
	ret = rtk_hal_crypto_set_dstdesc(req, ctx, base);
	if (ret) {
		DBG_CRYPTO_ERR("Do cipher req: setup desitination descriptor fail\n");
		return ret;
	}

	// step 2: Setup source descriptor
	//   step 2-1: prepare Key & IV array & command setting packet
	ret = rtk_setup_key_iv_cmd(req, ctx, base);
	if (ret)
		return ret;

	//   step 2-2: prepare Data1 ~ DataN
	ret = rtk_hal_crypto_set_srcdesc(req, ctx, base);
	if (ret)
		return ret;

	//printk("base->ipscsr_err_stats_reg = 0x%x", base->ipscsr_err_stats_reg);

	//printk("rtk_cryp_cipher_one_req ok\n");
cipherReqEnd:
	rtk_cryp_unprepare_cipher_req(engine, areq);

	return 0;
}

static void rtk_crypto_init(CRYPTO_Type *base, void __iomem *ctrl_port_reg, u32 slave_port)
{
	crypto_ipscsr_int_mask_reg_t mask;
	crypto_ipscsr_debug_reg_t status_reg;
	crypto_ipscsr_reset_isr_conf_reg_t reset_reg;
	crypto_ipscsr_swap_burst_reg_t val;
	u32 reg;

	//Override crypto core 0 or 1 slave port ARPROT
	reg = readl(ctrl_port_reg);
	writel(slave_port | reg, ctrl_port_reg);

	//Crypto engine : Software Reset
	reset_reg.w = 0;
	reset_reg.b.ipsec_rst = 1;

	writel(reset_reg.w, &base->ipscsr_reset_isr_conf_reg);

#if 0//(LEXRA_BIG_ENDIAN == 1)
	crypto_ipscsr_swap_burst_reg_t val;
	val.w = 0;
	val.b.set_swap = 1;
	//val.b.dma_burst_length = burstSize;
	val.b.dma_burst_length = 16;
	(pcrypto->ipscsr_swap_burst_reg) = val.w;
#else
	val.w = 0;
	val.b.key_iv_swap = 1;
	val.b.key_pad_swap = 1;
	val.b.hash_inital_value_swap = 1;
	val.b.dma_in_little_endian = 1;
	val.b.data_out_little_endian = 1;
	val.b.mac_out_little_endian = 1;
	val.b.dma_burst_length = 16;
	printk("base->ipscsr_swap_burst_reg = 0x%x\n", base->ipscsr_swap_burst_reg);
	writel(val.w, &base->ipscsr_swap_burst_reg);
	printk("base->ipscsr_swap_burst_reg = 0x%x\n", base->ipscsr_swap_burst_reg);
#endif
	//irq mask
	mask.w = 0;
	//mask.b.cmd_ok_m = 0;
	writel(mask.w, &base->ipscsr_int_mask_reg);
	printk("base->ipscsr_int_mask_reg = %d\n",base->ipscsr_int_mask_reg);
	//enable clock
	status_reg.w = 0;
	status_reg.b.arbiter_mode = 1;
	status_reg.b.engine_clk_en = 1;
	writel(status_reg.w, &base->ipscsr_debug_reg);
}

int rtk_algs_register(void)
{
	int ret = 0;

	ret = rtk_crypto_alg_register();
	if (ret) {
		DBG_CRYPTO_ERR("register crypto alg fail\n");
		return ret;
	}

	ret = rtk_hash_alg_register();
	if (ret) {
		DBG_CRYPTO_ERR("register hash alg fail\n");
		return ret;
	}

	ret = rtk_aead_alg_register();
	if (ret) {
		DBG_CRYPTO_ERR("register aead alg fail\n");
		return ret;
	}

	ret = rtk_gcm_alg_register();
	if (ret) {
		DBG_CRYPTO_ERR("register gcm alg fail\n");
		return ret;
	}

	return 0;
}

#if TASKLET_MODE
static irqreturn_t rtk_irq_handler(int irq, void *data)
{
	struct rtk_cryp *cryp = (struct rtk_cryp *)data;
	CRYPTO_Type *base = cryp->base_addr;

	if (base->ipscsr_reset_isr_conf_reg_b.cmd_ok)
	{
		DBG_CRYPTO_INFO("cmd ok!!!!!\n");
		base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;
	}
	else {
		DBG_CRYPTO_ERR("base->ipscsr_err_stats_reg = 0x%x", base->ipscsr_err_stats_reg);
		DBG_CRYPTO_ERR("cmd fail!!!!!\n");
		cryp->req_err = -EAGAIN;
		writel(base->ipscsr_err_stats_reg, &base->ipscsr_err_stats_reg);
	}

	tasklet_hi_schedule(&cryp->done_task);

	return IRQ_HANDLED;
}

static void rtk_done_task(unsigned long data)
{
	struct rtk_cryp *cryp = (struct rtk_cryp *)data;
	int err = cryp->req_err;
	u8 ihash[32];

	cryp->req_err = 0;

	if (cryp->aead_req) {
		struct crypto_aead *aead = crypto_aead_reqtfm(cryp->aead_req);
		struct rtk_aead_reqctx *rctx = aead_request_ctx(cryp->aead_req);
		struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);

		ctx->total_len = 0;
		if (rctx->first_phase) {
			rctx->first_phase = 0;
			if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
				scatterwalk_map_and_copy(ihash, cryp->aead_req->src, cryp->aead_req->assoclen + cryp->aead_req->cryptlen - aead->authsize, aead->authsize, 0);
				if (crypto_memneq(ihash, ctx->digest_result, aead->authsize)) {
					rctx->wait->err = -EBADMSG;
					complete(&rctx->wait->completion);
					crypto_finalize_aead_request(cryp->engine, cryp->aead_req, -EBADMSG);
				}
				else
					complete(&rctx->wait->completion);
			}
			else {
				complete(&rctx->wait->completion);
			}
		}
		else {
			if ((rctx->mode & FLG_GCM) && (rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
				scatterwalk_map_and_copy(ihash, cryp->aead_req->src, cryp->aead_req->assoclen + cryp->aead_req->cryptlen - aead->authsize, aead->authsize, 0);

				if (crypto_memneq(ihash, ctx->digest_result, aead->authsize)) {
					err = -EBADMSG;
				}
			}
			crypto_finalize_aead_request(cryp->engine, cryp->aead_req, err);
		}
	}
	else if (cryp->cipher_req)
		crypto_finalize_skcipher_request(cryp->engine, cryp->cipher_req, err);
	else if (cryp->hash_req) {
		struct crypto_ahash *ahash = crypto_ahash_reqtfm(cryp->hash_req);
		struct rtk_hash_reqctx *rctx = ahash_request_ctx(cryp->hash_req);
		struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);

		if (rctx->mode & FLG_FINAL) {
			ctx->total_len = 0;
			ctx->last_remain_len = 0;
			memcpy(cryp->hash_req->result, ctx->digest_result, crypto_ahash_digestsize(ahash));
		}
		else if ((rctx->mode & HASH_OP_MASK) == FLG_UPDATE) {
			memcpy(ctx->src_buf, ctx->tmp_src_buf, ctx->restlen);
			ctx->last_remain_len = ctx->restlen;
		}
		if (ctx->dma_unmap_flag) {
			dma_unmap_sg(cryp->dev, cryp->hash_req->src, sg_nents(cryp->hash_req->src), DMA_TO_DEVICE);
			ctx->dma_unmap_flag = 0;
		}
		crypto_finalize_hash_request(cryp->engine, cryp->hash_req, err);
	}
}
#else
static irqreturn_t rtk_irq_thread(int irq, void *data)
{
	struct rtk_cryp *cryp = (struct rtk_cryp *)data;
	//CRYPTO_Type *base = cryp->base_addr;
	int err = cryp->req_err;
	u8 ihash[32];

	cryp->req_err = 0;
	
	
	if (cryp->aead_req) {
		struct crypto_aead *aead = crypto_aead_reqtfm(cryp->aead_req);
		struct rtk_aead_reqctx *rctx = aead_request_ctx(cryp->aead_req);
		struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
		if (rctx->first_phase) {
			rctx->first_phase = 0;
			if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
				scatterwalk_map_and_copy(ihash, cryp->aead_req->src, cryp->aead_req->assoclen + cryp->aead_req->cryptlen - aead->authsize, aead->authsize, 0);
				if (crypto_memneq(ihash, ctx->digest_result, aead->authsize)) {
					rctx->wait->err = -EBADMSG;
					complete(&rctx->wait->completion);
					crypto_finalize_aead_request(cryp->engine, cryp->aead_req, -EBADMSG);
				}
				else
					complete(&rctx->wait->completion);
			}
			else {
				complete(&rctx->wait->completion);
			}
		}
		else {
			if ((rctx->mode & FLG_GCM) && (rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
				scatterwalk_map_and_copy(ihash, cryp->aead_req->src, cryp->aead_req->assoclen + cryp->aead_req->cryptlen - aead->authsize, aead->authsize, 0);

				if (crypto_memneq(ihash, ctx->digest_result, aead->authsize)) {
					err = -EBADMSG;
				}
			}
			crypto_finalize_aead_request(cryp->engine, cryp->aead_req, err);
		}
	}
	else if (cryp->cipher_req)
		crypto_finalize_skcipher_request(cryp->engine, cryp->cipher_req, err);
	else if (cryp->hash_req) {
		struct crypto_ahash *ahash = crypto_ahash_reqtfm(cryp->hash_req);
		struct rtk_hash_reqctx *rctx = ahash_request_ctx(cryp->hash_req);
		struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);

		if (rctx->mode & FLG_FINAL) {
			ctx->total_len = 0;
			ctx->last_remain_len = 0;
			memcpy(cryp->hash_req->result, ctx->digest_result, crypto_ahash_digestsize(ahash));
		}
		else if ((rctx->mode & HASH_OP_MASK) == FLG_UPDATE) {
			memcpy(ctx->src_buf, ctx->tmp_src_buf, ctx->restlen);
			ctx->last_remain_len = ctx->restlen;
		}
		if (ctx->dma_unmap_flag) {
			dma_unmap_sg(cryp->dev, cryp->hash_req->src, sg_nents(cryp->hash_req->src), DMA_TO_DEVICE);
			ctx->dma_unmap_flag = 0;
		}
		crypto_finalize_hash_request(cryp->engine, cryp->hash_req, err);
	}

	return IRQ_HANDLED;
}

static irqreturn_t rtk_irq_handler(int irq, void *data)
{
	struct rtk_cryp *cryp = (struct rtk_cryp *)data;
	CRYPTO_Type *base = cryp->base_addr;

	if (base->ipscsr_reset_isr_conf_reg_b.cmd_ok)
	{
		DBG_CRYPTO_INFO("cmd ok!!!!!\n");
		base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;
	}
	else {
		DBG_CRYPTO_ERR("base->ipscsr_err_stats_reg = 0x%x", base->ipscsr_err_stats_reg);
		DBG_CRYPTO_ERR("cmd fail!!!!!\n");
		writel(base->ipscsr_err_stats_reg, &base->ipscsr_err_stats_reg);
		cryp->req_err = -EAGAIN;
	}
	return IRQ_WAKE_THREAD;
}
#endif

static void __iomem *ctrl_port_reg;
static bool alg_registed = 0;
static int rtk_crypto_probe(struct platform_device *pdev)
{
	//struct rtk_cryp	*cryp;
	CRYPTO_Type *base = NULL;

	int ret = 0;
	int irq = 0;
	unsigned int slave_port = 0;
	printk("!!!!!!!!!!!!!!!!!!!!!!rtk crypto probe !!!!!!!!!!!!!!!!!!!!!!!!!!\n");

	//iomap
	crypto = devm_kzalloc(&pdev->dev, sizeof(*crypto), GFP_KERNEL);
	if (!crypto)
		return -ENOMEM;

	crypto->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if(ctrl_port_reg == NULL)
		ctrl_port_reg = devm_platform_ioremap_resource(pdev, 1);
	
	ret = of_property_read_u32(pdev->dev.of_node, "pe_ctrl_crypto_prot",
		&slave_port);

	if (ret != 0) {
		DBG_CRYPTO_ERR("can't find pe_ctrl_crypto_prot, set default slave_port 0x10\n");
		slave_port = 0x10;
	}

	crypto->dev = &pdev->dev;
	base = (CRYPTO_Type *)crypto->base_addr;

	//irq
	irq = platform_get_irq(pdev, 0);

	if (irq < 0) {
		DBG_CRYPTO_ERR("unable to get irq from Device Tree.\n");
		return -ENODEV;
	}

#if TASKLET_MODE
	tasklet_init(&crypto->done_task, rtk_done_task, (unsigned long)crypto);
	ret = devm_request_irq(&pdev->dev, irq, rtk_irq_handler, 0, dev_name(crypto->dev), crypto);
#else
	ret = devm_request_threaded_irq(&pdev->dev, irq, rtk_irq_handler, rtk_irq_thread, IRQF_ONESHOT, dev_name(crypto->dev), crypto);
#endif

	if (ret) {
		DBG_CRYPTO_ERR("unable to request irq.\n");
		goto err;		
	}

	//engine init
	rtk_crypto_init(base, ctrl_port_reg, slave_port);

	crypto->engine = crypto_engine_alloc_init(crypto->dev, 1);
	if (!crypto->engine) {
		DBG_CRYPTO_ERR("Could not init crypto engine\n");
		return -ENOMEM;
	}

	ret = crypto_engine_start(crypto->engine);
	if (ret) {
		DBG_CRYPTO_ERR("Could not start crypto engine\n");
		goto err_engine;
	}

	//register alg
	if(!alg_registed) {
		ret = rtk_algs_register();
		if (ret) {
			DBG_CRYPTO_ERR("Register algs fail\n");
			goto err_engine;
		}
		alg_registed = 1;
	}

	printk("!!!!!!!!!!!!!!!!!!!!!!rtk crypto probe ok!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
	return 0;

err_engine:
	crypto_engine_exit(crypto->engine);
err:
	return ret;
}

static int rtk_crypto_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id of_crypto_id[] = {
	{ .compatible = "realtek,8277c-crypto" },
	{ .compatible = "realtek,mercury-crypto" },
	{},
};
MODULE_DEVICE_TABLE(of, of_crypto_id);

static struct platform_driver rtk_crypto_driver = {
        .probe = rtk_crypto_probe,
        .remove = rtk_crypto_remove,
        .driver = {
                   .name = "rtk-crypto",
                   .of_match_table = of_crypto_id,
        },
};

module_platform_driver(rtk_crypto_driver);
MODULE_LICENSE("GPL");
