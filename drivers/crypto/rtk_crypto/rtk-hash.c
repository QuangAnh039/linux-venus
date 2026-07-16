#include "rtk-core.h"

static int rtk_update_final_set_desc(struct ahash_request *req, int op, u32 sg_len)
{
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(req);
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);
	CRYPTO_Type *base = NULL;
	rtl_crypto_srcdesc_t src_desc;
	rtl_crypto_dstdesc_t dst_desc;
	struct rtk_cryp *cryp = ctx->cryp;
	rtl_crypto_cl_t *cmd_ptr;
	int ret;
	struct scatterlist *sg;
	int nr_sgs, i;
	u32 len;
	dma_addr_t addr;

	base = cryp->base_addr;

	//setup dst desc
	if (op == FLG_FINAL) {

		dst_desc.w = 0;
		dst_desc.auth.ws  = 1;
		dst_desc.auth.fs = 1;
		dst_desc.auth.ls = 1;
		dst_desc.auth.adl = crypto_ahash_digestsize(ahash);

		ret = set_dst_desc(base, dst_desc.w, ctx->p_digest_result);

		if (ret) {
			DBG_CRYPTO_ERR("set dst desc fail\n");			
			return ret;
		}
	}

	//clear command ok & error interrupts
	base->ipscsr_err_stats_reg = 0xFFFF;
	base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;

	//prepare command setting buffer
	cmd_ptr = (rtl_crypto_cl_t *)ctx->cmd_setting;
	memset((u8*)cmd_ptr, 0, 32);

	cmd_ptr->engine_mode = 1; //hash only
	cmd_ptr->hmac_seq_hash = 1;

	if (ctx->total_len == 0) {
		cmd_ptr->hmac_seq_hash_first = 1;
	}

	if (op == FLG_FINAL) {
		ctx->total_len = ctx->total_len + ctx->last_remain_len +sg_len;
		cmd_ptr->hmac_seq_hash_last = 1;
		cmd_ptr->hmac_seq_hash_no_wb = 0;
		cmd_ptr->enc_last_data_size = ctx->total_len % 16; // pshuang need to check?
		ctx->hash_padding_len = (16 - cmd_ptr->enc_last_data_size) % 16;
		cmd_ptr->ap0 = ctx->total_len * 8;
		cmd_ptr->enl = (ctx->last_remain_len + sg_len + 15)/16;
	}
	if (op == FLG_UPDATE) {
		ctx->total_len = ctx->total_len + ctx->last_remain_len + sg_len;
		cmd_ptr->hmac_seq_hash_last = 0;
		cmd_ptr->hmac_seq_hash_no_wb = 1;
		cmd_ptr->enl = (ctx->last_remain_len + sg_len)/64;
		ctx->hash_padding_len = 0;
	}

	cmd_ptr->habs = 1;
	switch (rctx->mode & HASH_TYPE_MASK_BLOCK) 
	{
		case FLG_MD5:
			cmd_ptr->hibs = 1;
			cmd_ptr->hobs = 1;
			cmd_ptr->hkbs = 1;
			break;		
		case FLG_SHA1:
			cmd_ptr->hmac_mode = 1;
			break;
		case FLG_SHA2_224:
			cmd_ptr->hmac_mode = 2;
			break;
		case FLG_SHA2_256:
			cmd_ptr->hmac_mode = 3;
			break;
	}

	//HMAC algs?
	if (rctx->mode & FLG_HMAC) {
		cmd_ptr->hmac_en = 1;
		cmd_ptr->ap0 = cmd_ptr->ap0 + (64 * 8);
	}

	cmd_ptr->icv_total_length = 0x40;

	//set command setting
	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	src_desc.b.cl = 3;
	if (op == FLG_FINAL)
		src_desc.b.ap = 0x01;

	ret = set_src_desc(base, src_desc.w, ctx->p_cmd_setting);

	if (ret) {
		DBG_CRYPTO_ERR("update: Set command setting to src fifo fail\n");
		return ret;
	}

	//set HMAC keypad
	if (rctx->mode & FLG_HMAC) {
		src_desc.w = 0;
		src_desc.b.rs = 1;
		src_desc.b.fs = 1;
		src_desc.b.keypad_len = 128/4;
		ret = set_src_desc(base, src_desc.w, ctx->p_keypad);
		if (ret) {
			DBG_CRYPTO_ERR("update: Set HMAC keypad to src fifo fail\n");
			return ret;
		}
	}

	//set data fifo
	if (ctx->last_remain_len) {
		src_desc.w = 0;
		src_desc.d.rs = 1;
		if (ctx->hash_padding_len == 0 && sg_len == 0)
			src_desc.d.ls = 1;

		src_desc.d.enl = ctx->last_remain_len;

		ret = set_src_desc(base, src_desc.w, ctx->p_src_buf);
		if (ret) {
			DBG_CRYPTO_ERR("update: Set data to src fifo fail\n");
			return ret;
		}
	}

	if (sg_len) {
		nr_sgs = dma_map_sg(cryp->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Invalid src sg number %d\n", nr_sgs);
			return -EINVAL;
		}

		for_each_sg(req->src, sg, nr_sgs, i) {
			src_desc.w = 0;
			src_desc.d.rs = 1;

			len = sg_dma_len(sg);
			addr = sg_dma_address(sg);

			while (len > MAX_CRYPTO_SRC_LEN) {

				if (sg_len <= MAX_CRYPTO_SRC_LEN) {
					src_desc.d.enl = sg_len;
					//len = 0;
					if (ctx->hash_padding_len == 0)
						src_desc.d.ls = 1;

					ret = set_src_desc(base, src_desc.w, addr);
					if (ret)
						return ret;
					goto set_data_done;
				}
				else {
					src_desc.d.enl = MAX_CRYPTO_SRC_LEN;
				}

				ret = set_src_desc(base, src_desc.w, addr);
				if (ret)
					return ret;
				addr = addr + MAX_CRYPTO_SRC_LEN;
				len = len - MAX_CRYPTO_SRC_LEN;
				sg_len = sg_len - MAX_CRYPTO_SRC_LEN;

				src_desc.w = 0;
				src_desc.d.rs = 1;
			}

			if (len) {
				if ((sg_len <= len)) {
					src_desc.d.enl = sg_len;
					if (ctx->hash_padding_len == 0)
						src_desc.d.ls = 1;
					ret = set_src_desc(base, src_desc.w, addr);
					if (ret)
						return ret;

					goto set_data_done;
				}
				else {
					if (sg_is_last(sg) && (ctx->hash_padding_len == 0))
						src_desc.d.ls = 1;
					src_desc.d.enl = len;
					sg_len = sg_len - len;

					ret = set_src_desc(base, src_desc.w, addr);
					if (ret)
						return ret;
				}
			}
		}
	}

set_data_done:

	if (ctx->hash_padding_len) {
		src_desc.w = 0;
		src_desc.d.rs = 1;
		src_desc.d.enl = ctx->hash_padding_len;
		src_desc.d.ls = 1;

		ret = set_src_desc(base, src_desc.w, ctx->p_hash_padding);
		if (ret) {
			DBG_CRYPTO_ERR("update: Set data to src fifo fail\n");
			return ret;
		}
	}

	return 0;
}

int rtk_hash_cipher_one_req(struct crypto_engine *engine, void *areq)
{
	struct ahash_request *req = container_of(areq, struct ahash_request, base);
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(req);
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);
	struct rtk_cryp *cryp = ctx->cryp;
	CRYPTO_Type *base = NULL;
	u32 src_len, buf_rest_bytes, rest_src_len, restlen, bodylen;

	if (!cryp) {
		DBG_CRYPTO_ERR("Do cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	base = cryp->base_addr;

	cryp->aead_req = NULL;
	cryp->cipher_req = NULL;
	cryp->hash_req = req;

	if (rctx->mode & FLG_FINAL) {
		//finup
		if (rctx->mode & FLG_UPDATE) {
			src_len = req->nbytes;
			ctx->dma_unmap_flag = 1;
			// set src_buf & req->src to engine
			return rtk_update_final_set_desc(req, FLG_FINAL, src_len);
		}
		else { //only final
			// set src_buf to engine
			return rtk_update_final_set_desc(req, FLG_FINAL, 0);
		}

	}
	else if ((rctx->mode & HASH_OP_MASK) == FLG_UPDATE) {
		src_len = req->nbytes;
		buf_rest_bytes = MAX_HASH_MSG_SIZE - ctx->last_remain_len;
		rest_src_len = src_len - buf_rest_bytes;

		restlen = rest_src_len % MAX_HASH_MSG_SIZE;
		bodylen = rest_src_len - restlen;

		if ((bodylen == 0) && (restlen > 0)) { // there are only restlen, leave next round to handle seq_buf
			sg_pcopy_to_buffer(req->src, sg_nents(req->src), ctx->tmp_src_buf, restlen, src_len - restlen);
			ctx->restlen = restlen;
			if (ctx->last_remain_len != MAX_HASH_MSG_SIZE) {
				ctx->dma_unmap_flag = 1;
				return rtk_update_final_set_desc(req, FLG_UPDATE, buf_rest_bytes);
			}
			else
				return rtk_update_final_set_desc(req, FLG_UPDATE, 0);
		}
		else if ((bodylen > 0) && (restlen > 0)) { // there are MAX_HASH_MSG_SIZEx messages and restlen, leave next round to handle restlen
			sg_pcopy_to_buffer(req->src, sg_nents(req->src), ctx->tmp_src_buf, restlen, src_len - restlen);
			ctx->restlen = restlen;
			ctx->dma_unmap_flag = 1;
			return rtk_update_final_set_desc(req, FLG_UPDATE, src_len - restlen);
		}
		else if ((bodylen == MAX_HASH_MSG_SIZE) && (restlen == 0)) { // there are only MAX_HASH_MSG_SIZE byte message, backup the bodylen into seq_buf, leave next round to handle it
			sg_pcopy_to_buffer(req->src, sg_nents(req->src), ctx->tmp_src_buf, bodylen, src_len - bodylen);
			ctx->restlen = MAX_HASH_MSG_SIZE;

			if (ctx->last_remain_len != MAX_HASH_MSG_SIZE) {
				ctx->dma_unmap_flag = 1;
				return rtk_update_final_set_desc(req, FLG_UPDATE, buf_rest_bytes);
			}
			else
				return rtk_update_final_set_desc(req, FLG_UPDATE, 0);
		}
		else if ((bodylen > MAX_HASH_MSG_SIZE) && (restlen == 0)) { // there are only MAX_HASH_MSG_SIZEx messages, leave next round to handle full data of seq_buf
			sg_pcopy_to_buffer(req->src, sg_nents(req->src), ctx->tmp_src_buf, MAX_HASH_MSG_SIZE, src_len - MAX_HASH_MSG_SIZE );
			ctx->restlen = MAX_HASH_MSG_SIZE;
			ctx->dma_unmap_flag = 1;
			return rtk_update_final_set_desc(req, FLG_UPDATE, src_len - MAX_HASH_MSG_SIZE);
		}

	}

	return 0;
}

static int rtk_hash_init_tfm_algs(struct crypto_ahash *tfm, u32 hmac)
{
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	int ret;

	memset(ctx, 0, sizeof(*ctx));

	crypto_ahash_set_reqsize(tfm, sizeof(struct rtk_hash_reqctx));

	ctx->cryp = crypto;

	//16383 is max value, but not 64bytes alignment, so set 16320 to MAX_HASH_MSG_SIZE
	ctx->src_buf = dma_alloc_coherent(ctx->cryp->dev, MAX_HASH_MSG_SIZE, &ctx->p_src_buf, GFP_KERNEL);
	if (!ctx->src_buf) {
		DBG_CRYPTO_ERR("dma_alloc_coherent src_buf fail\n");
		return -ENOMEM;
	}

	ctx->digest_result = dma_alloc_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, &ctx->p_digest_result, GFP_KERNEL);
	if (!ctx->digest_result) {
		DBG_CRYPTO_ERR("dma_alloc_coherent digest_result fail\n");
		ret = -ENOMEM;
		goto err_digest;
	}

	ctx->cmd_setting = dma_alloc_coherent(ctx->cryp->dev, 32, &ctx->p_cmd_setting, GFP_KERNEL);
	if (!ctx->cmd_setting) {
		DBG_CRYPTO_ERR("dma_alloc_coherent command setting buffer fail\n");
		ret = -ENOMEM;
		goto err_cmd;
	}

	ctx->hash_padding = dma_alloc_coherent(ctx->cryp->dev, 16, &ctx->p_hash_padding, GFP_KERNEL);
	if (!ctx->hash_padding) {
		DBG_CRYPTO_ERR("dma_alloc_coherent hash_padding fail\n");
		ret = -ENOMEM;
		goto err_padding;
	}

	if (hmac) {
		ctx->keypad = dma_alloc_coherent(ctx->cryp->dev, 128, &ctx->p_keypad, GFP_KERNEL);
		if (!ctx->keypad) {
			DBG_CRYPTO_ERR("dma_alloc_coherent keypad fail\n");
			ret = -ENOMEM;
			goto err_keypad;
		}
	}

	//ctx->enginectx.op.do_one_request = rtk_hash_cipher_one_req;
	
	return 0;

err_keypad:
	dma_free_coherent(ctx->cryp->dev, 16, ctx->hash_padding, ctx->p_hash_padding);
err_padding:
	dma_free_coherent(ctx->cryp->dev, 32, ctx->cmd_setting, ctx->p_cmd_setting);
err_cmd:
	dma_free_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, ctx->digest_result, ctx->p_digest_result);
err_digest:
	dma_free_coherent(ctx->cryp->dev, MAX_HASH_MSG_SIZE, ctx->src_buf, ctx->p_src_buf);

	return ret;

}

static int rtk_hash_init_tfm(struct crypto_ahash *tfm)
{
	return rtk_hash_init_tfm_algs(tfm ,0);
}

static int rtk_hash_hmac_init_tfm(struct crypto_ahash *tfm)
{
	return rtk_hash_init_tfm_algs(tfm ,1);
}

static void rtk_hash_exit_tfm(struct crypto_ahash *tfm)
{
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(tfm);

	if (ctx->digest_result)
		dma_free_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, ctx->digest_result, ctx->p_digest_result);

	if (ctx->src_buf)
		dma_free_coherent(ctx->cryp->dev, MAX_HASH_MSG_SIZE, ctx->src_buf, ctx->p_src_buf);

	if (ctx->cmd_setting)
		dma_free_coherent(ctx->cryp->dev, 32, ctx->cmd_setting, ctx->p_cmd_setting);

	if (ctx->hash_padding)
		dma_free_coherent(ctx->cryp->dev, 16, ctx->hash_padding, ctx->p_hash_padding);

	if (ctx->keypad)
		dma_free_coherent(ctx->cryp->dev, 128, ctx->keypad, ctx->p_keypad);
}

static int rtk_hash_init_algs(struct ahash_request *req, u32 hmac)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	int digcnt;

	digcnt = crypto_ahash_digestsize(tfm);
	
	switch(digcnt) {
		case MD5_DIGEST_SIZE:
			rctx->mode = FLG_MD5;
			break;
		case SHA1_DIGEST_SIZE:
			rctx->mode = FLG_SHA1;
			break;
		case SHA224_DIGEST_SIZE:
			rctx->mode = FLG_SHA2_224;
			break;
		case SHA256_DIGEST_SIZE:
			rctx->mode = FLG_SHA2_256;
			break;
		default:
			return -EINVAL;
	}

	if (hmac)
		rctx->mode = rctx->mode | FLG_HMAC;

	return 0;
}

static int rtk_hash_init(struct ahash_request *req)
{
	return rtk_hash_init_algs(req, 0);
}

static int rtk_hash_hmac_init(struct ahash_request *req)
{
	return rtk_hash_init_algs(req, 1);
}

static int rtk_hash_update(struct ahash_request *req)
{
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(req);
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);
	struct rtk_cryp *cryp = ctx->cryp;
	u32 diglen = req->nbytes;

	if (!req->nbytes)
		return 0;

	if (!cryp) {
		DBG_CRYPTO_ERR("update: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	rctx->mode = rctx->mode & ~FLG_FINAL & ~FLG_UPDATE;
	rctx->mode = rctx->mode | FLG_UPDATE;

	if (ctx->last_remain_len + diglen <= MAX_HASH_MSG_SIZE) {
		sg_copy_to_buffer(req->src, sg_nents(req->src), ctx->src_buf + ctx->last_remain_len, diglen);
		ctx->last_remain_len = ctx->last_remain_len + diglen;
		return 0;
	}

	return crypto_transfer_hash_request_to_engine(cryp->engine, req);
}

static int generic_hmac(u8 *src, u8 *result, const char *algs_name, struct rtk_hash_tfm_ctx *ctx)
{
	struct crypto_shash *hash;
	struct sdesc *sdesc;
	int ret;

	hash = crypto_alloc_shash(algs_name, 0, 0);
	if (IS_ERR(hash)) {
		ret = PTR_ERR(hash);
		DBG_CRYPTO_ERR("alloc generic_hmac hash fail\n");
		return ret;
	}

	sdesc = kmalloc(sizeof(struct shash_desc) + crypto_shash_descsize(hash), GFP_KERNEL);
	if (!sdesc) {
		ret = -ENOMEM;
		goto err_shash;
	}

	sdesc->shash.tfm = hash;

	ret = crypto_shash_setkey(hash, ctx->keypad, ctx->keypad_len);
	if (ret) {
		DBG_CRYPTO_ERR("generic_hmac setkey fail\n");
		goto err_shash;
	}

	ret = crypto_shash_init(&sdesc->shash);
	if (ret) {
		DBG_CRYPTO_ERR("generic_hmac init fail\n");
		goto err_shash;
	}

	ret = crypto_shash_digest(&sdesc->shash, src, 0, result);
	
err_shash:
	crypto_free_shash(hash);

	if (sdesc)
		kfree(sdesc);

	return ret;
}

static int rtk_hash_final(struct ahash_request *req)
{
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(req);
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);
	struct rtk_cryp *cryp = ctx->cryp;

	if (!cryp) {
		DBG_CRYPTO_ERR("final: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	if (!req->result) {
		DBG_CRYPTO_ERR("final: req result is null\n")
		return -EINVAL;
	}

	if (ctx->total_len == 0 && ctx->last_remain_len == 0) {
		switch (rctx->mode & HASH_TYPE_MASK_BLOCK) 
		{
			case FLG_MD5:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(md5-generic)", ctx);
				else
					memcpy(req->result, md5_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;		
			case FLG_SHA1:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha1-generic)", ctx);
				else
					memcpy(req->result, sha1_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
			case FLG_SHA2_224:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha224-generic)", ctx);
				else
					memcpy(req->result, sha224_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
			case FLG_SHA2_256:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha256-generic)", ctx);
				else
					memcpy(req->result, sha256_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
		}
		return 0;
	}

	rctx->mode = rctx->mode & ~FLG_FINAL & ~FLG_UPDATE;
	rctx->mode = rctx->mode | FLG_FINAL;

	return crypto_transfer_hash_request_to_engine(cryp->engine, req);
}

static int rtk_hash_finup(struct ahash_request *req)
{
	struct rtk_hash_reqctx *rctx = ahash_request_ctx(req);
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(req);
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(ahash);
	struct rtk_cryp *cryp = ctx->cryp;

	if (!cryp) {
		DBG_CRYPTO_ERR("finup: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	if (!req->result) {
		DBG_CRYPTO_ERR("finup: req result is null\n")
		return -EINVAL;
	}

	if (ctx->total_len == 0 && ctx->last_remain_len == 0 && req->nbytes == 0) {
		switch (rctx->mode & HASH_TYPE_MASK_BLOCK) 
		{
			case FLG_MD5:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(md5-generic)", ctx);
				else
					memcpy(req->result, md5_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;		
			case FLG_SHA1:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha1-generic)", ctx);
				else
					memcpy(req->result, sha1_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
			case FLG_SHA2_224:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha224-generic)", ctx);
				else
					memcpy(req->result, sha224_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
			case FLG_SHA2_256:
				if (rctx->mode & FLG_HMAC)
					return generic_hmac(sg_virt(req->src), req->result, "hmac(sha256-generic)", ctx);
				else
					memcpy(req->result, sha256_zero_message_hash, crypto_ahash_digestsize(ahash));
				break;
		}

		return 0;
	}

	rctx->mode = rctx->mode | FLG_FINAL | FLG_UPDATE;

	return crypto_transfer_hash_request_to_engine(cryp->engine, req);
}


static int rtk_hash_hmac_digest(struct ahash_request *req)
{
	return rtk_hash_hmac_init(req) ?: rtk_hash_finup(req);
}

static int rtk_hash_digest(struct ahash_request *req)
{
	return rtk_hash_init(req) ?: rtk_hash_finup(req);
}

static int rtk_hash_export(struct ahash_request *req, void *out)
{
	struct rtk_hash_reqctx *ctx = ahash_request_ctx(req);

	memcpy(out, ctx, sizeof(*ctx));
	return 0;
}

static int rtk_hash_import(struct ahash_request *req, const void *in)
{
	struct rtk_hash_reqctx *ctx = ahash_request_ctx(req);

	memcpy(ctx, in, sizeof(*ctx));
	return 0;
}

static int rtk_hash_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned int keylen, const char *algs_name)
{
	struct rtk_hash_tfm_ctx *ctx = crypto_ahash_ctx(tfm);
	struct scatterlist sg;
	u8 *ipad = &(ctx->keypad[0]);
	u8 *opad = &(ctx->keypad[64]);
	int ret = 0;
	struct crypto_ahash *key_tfm = NULL;
	struct ahash_request *areq = NULL;
	u8 buf[64] = {0};
	u8 *keydup = NULL;
	DECLARE_CRYPTO_WAIT(wait);

	if (keylen <= 0) {
		return -EINVAL;
	}

	memset(ctx->keypad, 0, 128);

	ctx->keypad_len = keylen;

	if (keylen > 64) {
		keydup = kmemdup(key, keylen, GFP_KERNEL);
		if (!keydup)
			return -ENOMEM;

		key_tfm = crypto_alloc_ahash(algs_name, 0, 0);
		if (IS_ERR(key_tfm)) {
			ret = PTR_ERR(key_tfm);
			goto err_tfm;
		}

		areq = ahash_request_alloc(key_tfm, GFP_KERNEL);
		if (!areq) {
			ret = -ENOMEM;
			goto err_req;
		}

		ahash_request_set_callback(areq, 0, crypto_req_done, &wait);
		sg_init_one(&sg, keydup, keylen);
		ahash_request_set_crypt(areq, &sg, buf, keylen);
		ret = crypto_wait_req(crypto_ahash_digest(areq), &wait);
		if (ret) {
			goto err_digest;
		}

		ctx->keypad_len = crypto_ahash_digestsize(crypto_ahash_reqtfm(areq));
		key = buf;
	}

	memcpy(ipad, key, ctx->keypad_len);
	memcpy(opad, key, ctx->keypad_len);

err_digest:
	if (areq)
		ahash_request_free(areq);
err_req:
	if (key_tfm)
		crypto_free_ahash(key_tfm);
err_tfm:
	if (keydup)
		kfree_sensitive(keydup);

	return ret;
}

static int rtk_hmac_md5_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned int keylen)
{
	return rtk_hash_setkey(tfm, key, keylen, "rtk-md5");
}

static int rtk_hmac_sha1_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned int keylen)
{
	return rtk_hash_setkey(tfm, key, keylen, "rtk-sha1");
}

static int rtk_hmac_sha224_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned int keylen)
{
	return rtk_hash_setkey(tfm, key, keylen, "rtk-sha224");
}

static int rtk_hmac_sha256_setkey(struct crypto_ahash *tfm, const u8 *key, unsigned int keylen)
{
	return rtk_hash_setkey(tfm, key, keylen, "rtk-sha256");
}

static struct ahash_engine_alg hash_algs[] = {
	{
		.base.init = rtk_hash_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_digest,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = MD5_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "md5",
				.cra_driver_name = "rtk-md5",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = MD5_HMAC_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_digest,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA1_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "sha1",
				.cra_driver_name = "rtk-sha1",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA1_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_digest,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA224_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "sha224",
				.cra_driver_name = "rtk-sha224",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA224_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_digest,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA256_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "sha256",
				.cra_driver_name = "rtk-sha256",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA256_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_hmac_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_hmac_digest,
		.base.setkey = rtk_hmac_md5_setkey,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_hmac_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = MD5_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "hmac(md5)",
				.cra_driver_name = "rtk-hmac-md5",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = MD5_HMAC_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_hmac_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_hmac_digest,
		.base.setkey = rtk_hmac_sha1_setkey,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_hmac_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA1_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "hmac(sha1)",
				.cra_driver_name = "rtk-hmac-sha1",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA1_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},	
	},
	{
		.base.init = rtk_hash_hmac_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_hmac_digest,
		.base.setkey = rtk_hmac_sha224_setkey,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_hmac_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA224_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "hmac(sha224)",
				.cra_driver_name = "rtk-hmac-sha224",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA224_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
	{
		.base.init = rtk_hash_hmac_init,
		.base.update = rtk_hash_update,
		.base.final = rtk_hash_final,
		.base.finup = rtk_hash_finup,
		.base.digest = rtk_hash_hmac_digest,
		.base.setkey = rtk_hmac_sha256_setkey,
		.base.export = rtk_hash_export,
		.base.import = rtk_hash_import,
		.base.init_tfm = rtk_hash_hmac_init_tfm,
		.base.exit_tfm = rtk_hash_exit_tfm,
		.base.halg = {
			.digestsize = SHA256_DIGEST_SIZE,
			.statesize = sizeof(struct rtk_hash_reqctx),
			.base = {
				.cra_name = "hmac(sha256)",
				.cra_driver_name = "rtk-hmac-sha256",
				.cra_priority = 400,
				.cra_flags = CRYPTO_ALG_ASYNC,
				.cra_blocksize = SHA256_BLOCK_SIZE,
				.cra_ctxsize = sizeof(struct rtk_hash_tfm_ctx),
				.cra_module = THIS_MODULE,
			}
		},
		.op = {
			.do_one_request = rtk_hash_cipher_one_req,
		},
	},
};

int rtk_hash_alg_register(void)
{
	int ret = 0;
	ret = crypto_engine_register_ahashes(hash_algs, ARRAY_SIZE(hash_algs));
	if (ret)
		goto fail;

	return 0;

fail:
	crypto_engine_unregister_ahashes(hash_algs, ARRAY_SIZE(hash_algs));

	return ret;

}

