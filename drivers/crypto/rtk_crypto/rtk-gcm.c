#include "rtk-core.h"

const u8 gcm_iv_tail[] __attribute__((aligned(64))) = {0x00, 0x00, 0x00, 0x01};

int rtk_gcm_prepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp; // need to check?
	CRYPTO_Type *base = NULL;
	rtl_crypto_cl_t *cmd_ptr;
	int nr_sgs;
	int nr_sgd;
	struct scatterlist *cipher_src, *cipher_dst;
	int ret;
	u32 cryptlen;
	u32 assoclen = req->assoclen;
	u32 authsize = aead->authsize;

	if (!cryp) {
		DBG_CRYPTO_ERR("Prepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;	
	}

	base = cryp->base_addr;

	//clear command ok & error interrupts
	base->ipscsr_err_stats_reg = 0xFFFF;
	base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - authsize;
	}
	else {
		cryptlen = req->cryptlen;
	}

	//prepare command setting buffer
	cmd_ptr = (rtl_crypto_cl_t *)ctx->cmd_setting;
	memset((u8*)cmd_ptr, 0, 32);

	//encrypt/decrypt
	if (rctx->mode & FLG_ENCRYPT)
		cmd_ptr->cipher_encrypt = 1;

	//AES setting
	cmd_ptr->cipher_eng_sel = 0;
	cmd_ptr->cabs = 1;

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
	}

	cmd_ptr->cipher_mode = rctx->mode & CIPHER_TYPE_MASK_BLOCK;

	cmd_ptr->enl = (cryptlen + 15)/AES_BLOCK_SIZE;
	cmd_ptr->enc_last_data_size = cryptlen % AES_BLOCK_SIZE;
	ctx->cryp_padding_len = (AES_BLOCK_SIZE - (cryptlen % AES_BLOCK_SIZE)) % AES_BLOCK_SIZE;

	cmd_ptr->header_total_len = (assoclen + 15)/AES_BLOCK_SIZE;
	cmd_ptr->aad_last_data_size = assoclen % AES_BLOCK_SIZE;
	ctx->a2eo_padding_len = (AES_BLOCK_SIZE - cmd_ptr->aad_last_data_size) % AES_BLOCK_SIZE;

	cmd_ptr->icv_total_length = 0x40;

	wmb();

	if (assoclen) {
		nr_sgs = dma_map_sg(cryp->dev, req->src, sg_nents_for_len(req->src, assoclen), DMA_TO_DEVICE);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid src/dst sg number %d\n", nr_sgs);
			return -EINVAL;
		}

		ctx->aad_nr_sgs = nr_sgs;
	}

	cipher_src = scatterwalk_ffwd(rctx->cipher_src, req->src, req->assoclen);
	rctx->crypto_src = cipher_src;

	if (req->src == req->dst) {
		nr_sgs = dma_map_sg(cryp->dev, cipher_src, sg_nents_for_len(cipher_src, cryptlen), DMA_BIDIRECTIONAL);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Invalid src sg number %d\n", nr_sgs);
			return -EINVAL;
		}

		nr_sgd = nr_sgs;
		rctx->crypto_dst = cipher_src;
		ctx->cipher_nr_sgs = nr_sgs;
		ctx->cipher_nr_sgd = nr_sgd;
	}
	else {	
		if (rctx->mode & FLG_ENCRYPT) {
			//copy assoc data
			SYNC_SKCIPHER_REQUEST_ON_STACK(skreq, ctx->null);
			skcipher_request_set_sync_tfm(skreq, ctx->null);
			skcipher_request_set_callback(skreq, aead_request_flags(req), NULL, NULL);
			skcipher_request_set_crypt(skreq, req->src, req->dst, req->assoclen, NULL);

			ret = crypto_skcipher_encrypt(skreq);

			if (ret) {
				dma_unmap_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->assoclen), DMA_TO_DEVICE);
				return ret;
			}
		}

		cipher_dst = scatterwalk_ffwd(rctx->cipher_dst, req->dst, req->assoclen);

		rctx->crypto_dst = cipher_dst;

		nr_sgs = dma_map_sg(cryp->dev, cipher_src, sg_nents_for_len(cipher_src, cryptlen), DMA_TO_DEVICE);
		if (nr_sgs <= 0) {
			dma_unmap_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->assoclen), DMA_TO_DEVICE);
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid src sg number %d\n", nr_sgs);
			return -EINVAL;
		}
		ctx->cipher_nr_sgs = nr_sgs;

		nr_sgd = dma_map_sg(cryp->dev, cipher_dst, sg_nents_for_len(cipher_dst, cryptlen), DMA_FROM_DEVICE);
		if (nr_sgd <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid dst sg number %d\n", nr_sgd);
			dma_unmap_sg(cryp->dev, cipher_src, sg_nents_for_len(cipher_src, cryptlen), DMA_TO_DEVICE);
			dma_unmap_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->assoclen), DMA_TO_DEVICE);
			return -EINVAL;
		}

		ctx->cipher_nr_sgd = nr_sgd;
	}


	//store req in cryp
	cryp->cipher_req = NULL;
	cryp->hash_req = NULL;
	cryp->aead_req = req;

	rctx->first_phase = 0;

	//prepare iv array
	if (req->iv) {
		ctx->ivlen = 16;
		memcpy(ctx->iv, req->iv, GCM_AES_IV_SIZE);
		//gcm iv tail must be [0x00, 0x00, 0x00, 0x01]
		memcpy(&(ctx->iv[12]), gcm_iv_tail, 4);
	}

	return 0;
}

int rtk_gcm_unprepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp;
	u32 cryptlen;

	if (!cryp) {
		DBG_CRYPTO_ERR("Unprepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - aead->authsize;
	}
	else {
		cryptlen = req->cryptlen;
	}

	//unmap dma
	if (req->src == req->dst) {
		dma_unmap_sg(cryp->dev, rctx->crypto_src, sg_nents_for_len(rctx->crypto_src, cryptlen), DMA_BIDIRECTIONAL);
	}
	else {
		dma_unmap_sg(cryp->dev, rctx->crypto_src, sg_nents_for_len(rctx->crypto_src, cryptlen), DMA_TO_DEVICE);
		dma_unmap_sg(cryp->dev, rctx->crypto_dst, sg_nents_for_len(rctx->crypto_dst, cryptlen), DMA_FROM_DEVICE);
	}
	
	dma_unmap_sg(cryp->dev, req->src, sg_nents_for_len(req->src, req->assoclen), DMA_TO_DEVICE);

        if (rctx->mode & FLG_ENCRYPT) {
	        scatterwalk_map_and_copy(ctx->digest_result, req->dst, req->assoclen + req->cryptlen, aead->authsize, 1);
	}

	return 0;
}

static int rtk_gcm_set_dstdesc(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_dstdesc_t  dst_cipher_desc;
	rtl_crypto_dstdesc_t  dst_auth_desc;
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct scatterlist *sg;
	struct scatterlist *dst_sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen;
	u32 authsize = aead->authsize;

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - authsize;
	}
	else {
		cryptlen = req->cryptlen;
	}

	if (req->src == req->dst) {
		dst_sg = rctx->crypto_src;
	}
	else {
		dst_sg = rctx->crypto_dst;
	}

	for_each_sg(dst_sg, sg, ctx->cipher_nr_sgd, i) {
		dst_cipher_desc.w = 0;
		dst_cipher_desc.cipher.ws  = 1;
		dst_cipher_desc.cipher.enc = 1;

		if (i == 0)
			dst_cipher_desc.cipher.fs = 1;

		len = sg->length;
		addr = sg_dma_address(sg);

		// 16777215 bytes is the max size of dst enl
		while (len > MAX_CRYPTO_DST_LEN) {

			if (cryptlen <= MAX_CRYPTO_DST_LEN) {
				dst_cipher_desc.cipher.enl = cryptlen;
				//dst_cipher_desc.cipher.ls = 1;

				ret = set_dst_desc(base, dst_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_dst_enc_done;
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
				ret = set_dst_desc(base, dst_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_dst_enc_done;
			}
			else {
				dst_cipher_desc.cipher.enl = len;
			}

			cryptlen = cryptlen - len;

			ret = set_dst_desc(base, dst_cipher_desc.w, addr);
			if (ret)
				return ret;
		}
	}

set_dst_enc_done:

	dst_auth_desc.w = 0;
	dst_auth_desc.auth.fs = 1;
	dst_auth_desc.auth.ws  = 1;
	dst_auth_desc.auth.enc = 0;
	dst_auth_desc.auth.adl = 16;
	dst_auth_desc.auth.ls  = 1;

	ret = set_dst_desc(base, dst_auth_desc.w, ctx->p_digest_result);

	if (ret) {
		DBG_CRYPTO_ERR("set dst desc fail\n");			
		return ret;
	}

	return 0;
}

static int rtk_gcm_setup_key_iv_cmd(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_srcdesc_t src_desc;
	int ret;

	//set command setting
	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	src_desc.b.cl = 3;

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

	return 0;
}

static int rtk_gcm_setup_aad(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_srcdesc_t src_cipher_desc;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	struct scatterlist *sg;
	u32 assoclen = req->assoclen;

	for_each_sg(req->src, sg, ctx->aad_nr_sgs, i) {
		src_cipher_desc.w = 0;
		src_cipher_desc.d.rs = 1;

		len = sg_dma_len(sg);
		addr = sg_dma_address(sg);

		while (len > 16) {
			if (assoclen <= 16) {
				src_cipher_desc.d.a2eo = assoclen;

				ret = set_src_desc(base, src_cipher_desc.w, addr);
				if (ret)
					return ret;

				goto set_assoc_done;
			}
			src_cipher_desc.d.a2eo = 16;
			ret = set_src_desc(base, src_cipher_desc.w, addr);
			if (ret)
				return ret;

			addr = addr + 16;
			len = len - 16;
			assoclen = assoclen - 16;

			src_cipher_desc.w = 0;
			src_cipher_desc.d.rs = 1;
		}

		if (len) {
			if (assoclen <= len) {
				src_cipher_desc.d.a2eo = assoclen;
				ret = set_src_desc(base, src_cipher_desc.w, addr);

				if (ret)
					return ret;
				goto set_assoc_done;
			}
			src_cipher_desc.d.a2eo = len;

			assoclen = assoclen - len;

			ret = set_src_desc(base, src_cipher_desc.w, addr);
			if (ret)
				return ret;	
		}
	}

set_assoc_done:
	//a2eo padding
	if (ctx->a2eo_padding_len) {
		src_cipher_desc.w = 0;
		src_cipher_desc.d.rs = 1;
		src_cipher_desc.d.a2eo = ctx->a2eo_padding_len;

		ret = set_src_desc(base, src_cipher_desc.w, ctx->p_cryp_padding);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtk_gcm_set_srcdesc(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base)
{
	rtl_crypto_srcdesc_t src_cipher_desc;
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct scatterlist *sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen;
	u32 authsize = aead->authsize;

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - authsize;
	}
	else {
		cryptlen = req->cryptlen;
	}

	for_each_sg(rctx->crypto_src, sg, ctx->cipher_nr_sgs, i) {
		src_cipher_desc.w = 0;
		src_cipher_desc.d.rs = 1;

		len = sg->length;
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

/*
 * Length of input and output data:
 * Encryption case:
 *  INPUT  =   AssocData  ||   PlainText
 *          <- assoclen ->  <- cryptlen ->
 *          <------- total_in ----------->
 *
 *  OUTPUT =   AssocData  ||  CipherText  ||   AuthTag
 *          <- assoclen ->  <- cryptlen ->  <- authsize ->
 *          <---------------- total_out ----------------->
 *
 * Decryption case:
 *  INPUT  =   AssocData  ||  CipherText  ||  AuthTag
 *          <- assoclen ->  <--------- cryptlen --------->
 *                                          <- authsize ->
 *          <---------------- total_in ------------------>
 *
 *  OUTPUT =   AssocData  ||   PlainText
 *          <- assoclen ->  <- crypten - authsize ->
 *          <---------- total_out ----------------->
 */

int rtk_gcm_cipher_one_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp;
	CRYPTO_Type *base = NULL;
	int ret = 0;
	int err;
	DECLARE_CRYPTO_WAIT(wait);

	if (!cryp) {
		DBG_CRYPTO_ERR("Do cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	err = rtk_gcm_prepare_cipher_req(engine, areq);
	if (err)
		goto reqEnd;

	cryp->cipher_req = NULL;
	cryp->hash_req = NULL;
	cryp->aead_req = req;

	base = cryp->base_addr;

	// step 1: Setup desitination descriptor
	ret = rtk_gcm_set_dstdesc(req, ctx, base);
	if (ret) {
		DBG_CRYPTO_ERR("Do cipher req: setup desitination descriptor fail\n");
		//return ret;
		return 0;
	}

	// step 2: Setup source descriptor
	//   step 2-1: prepare Key & IV array & command setting packet
	ret = rtk_gcm_setup_key_iv_cmd(req, ctx, base);
	if (ret) {
		//return ret;
		return 0;
	}

	// step 2-2: setup aad
	if (req->assoclen) {
		ret = rtk_gcm_setup_aad(req, ctx, base);
		if (ret) {
			//return ret;
			return 0;
		}
	}

	// step 2-3: prepare Data1 ~ DataN
	ret = rtk_gcm_set_srcdesc(req, ctx, base);
	if (ret) {
		//return ret;
		return 0;
	}
reqEnd:
	rtk_gcm_unprepare_cipher_req(engine, areq);

	return 0;
}

static int rtk_init_gcm_tfm(struct crypto_aead *tfm)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(tfm);
	int ret = 0;
	struct crypto_sync_skcipher *null;

	crypto_aead_set_reqsize(tfm, sizeof(struct rtk_aead_reqctx));

	ctx->cryp = crypto;

	ctx->key = dma_alloc_coherent(ctx->cryp->dev, 32, &ctx->p_key, GFP_KERNEL);
	if (!ctx->key) {
		DBG_CRYPTO_ERR("dma_alloc_coherent key fail\n");
		return -ENOMEM;
	}

	ctx->iv = dma_alloc_coherent(ctx->cryp->dev, 16, &ctx->p_iv, GFP_KERNEL);
	if (!ctx->iv) {
		DBG_CRYPTO_ERR("dma_alloc_coherent iv fail\n");
		ret = -ENOMEM;
		goto err_iv;
	}

	ctx->cmd_setting = dma_alloc_coherent(ctx->cryp->dev, 32, &ctx->p_cmd_setting, GFP_KERNEL);
	if (!ctx->cmd_setting) {
		DBG_CRYPTO_ERR("dma_alloc_coherent command setting buffer fail\n");
		ret = -ENOMEM;
		goto err_cmd;
	}

	ctx->cryp_padding = dma_alloc_coherent(ctx->cryp->dev, 16, &ctx->p_cryp_padding, GFP_KERNEL);
	if (!ctx->cryp_padding) {
		DBG_CRYPTO_ERR("dma_alloc_coherent padding buffer fail\n");
		ret = -ENOMEM;
		goto err_cryp_padding;
	}

	ctx->digest_result = dma_alloc_coherent(ctx->cryp->dev, 16, &ctx->p_digest_result, GFP_KERNEL);
	if (!ctx->digest_result) {
		DBG_CRYPTO_ERR("dma_alloc_coherent digest_result fail\n");
		ret = -ENOMEM;
		goto err_digest;
	}

	null = crypto_get_default_null_skcipher();
	if (IS_ERR(null)) {
		ret = PTR_ERR(null);
		goto err_null_skcipher;
	}

	ctx->null = null;
	return 0;
	
err_null_skcipher:
	dma_free_coherent(ctx->cryp->dev, 16, ctx->digest_result, ctx->p_digest_result);
err_digest:
	dma_free_coherent(ctx->cryp->dev, 16, ctx->cryp_padding, ctx->p_cryp_padding);
err_cryp_padding:
	dma_free_coherent(ctx->cryp->dev, 32, ctx->cmd_setting, ctx->p_cmd_setting);
err_cmd:
	dma_free_coherent(ctx->cryp->dev, 16, ctx->iv, ctx->p_iv);
err_iv:
	dma_free_coherent(ctx->cryp->dev, 32, ctx->key, ctx->p_key);

	return ret;
}

static void rtk_exit_gcm_tfm(struct crypto_aead *tfm)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(tfm);
	struct rtk_cryp *cryp = ctx->cryp;

	if (!ctx->cryp) {
		DBG_CRYPTO_ERR("Get rtk_cryp device ERROR\n");
		return;
	}

        if (ctx->cmd_setting)
                dma_free_coherent(cryp->dev, 32, ctx->cmd_setting, ctx->p_cmd_setting);

        if (ctx->iv)
                dma_free_coherent(cryp->dev, 16, ctx->iv, ctx->p_iv);

        if (ctx->key)
                dma_free_coherent(cryp->dev, 32, ctx->key, ctx->p_key);

        if (ctx->cryp_padding)
                dma_free_coherent(cryp->dev, 16, ctx->cryp_padding, ctx->p_cryp_padding);

	if (ctx->digest_result)
		dma_free_coherent(ctx->cryp->dev, 16, ctx->digest_result, ctx->p_digest_result);

	crypto_put_default_null_skcipher();
}

static int rtk_gcm_setkey(struct crypto_aead *aead, const u8 *key, unsigned int keylen)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	int ret;

	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 && keylen != AES_KEYSIZE_256) {
		return -EINVAL;
	}

	ret = aes_expandkey(&ctx->actx, key, keylen);
	if(ret)
		return ret;

	memcpy(ctx->key, key, keylen);
	ctx->keylen = keylen;

	return 0;
}

static int generic_gcm(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, int op)
{
	struct crypto_aead *tfm = NULL;
	struct aead_request *areq = NULL;
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	int err;

	tfm = crypto_alloc_aead("gcm_base(ctr(aes-generic),ghash-generic)", 0, 0);
	if (IS_ERR(tfm)) {
		pr_err("Error allocating gcm-generic handle: %ld\n", PTR_ERR(tfm));
		return PTR_ERR(tfm);
	}

	areq = aead_request_alloc(tfm, GFP_KERNEL);

	if (!areq) {
		err = -ENOMEM;
		goto out;
	}

	err = crypto_aead_setkey(tfm, ctx->key, ctx->keylen);
	if (err) {
		pr_err("Error setting key: %d\n", err);
		goto out;
	}

	err = crypto_aead_setauthsize(tfm, aead->authsize);
	if (err) {
		pr_err("Error setting authsize: %d\n", err);
		goto out;
	}

	aead_request_set_callback(areq, req->base.flags, req->base.complete, req->base.data);
	aead_request_set_crypt(areq, req->src, req->dst, req->cryptlen, req->iv);
	aead_request_set_ad(areq, req->assoclen);
	
	if (op == FLG_ENCRYPT) {
		err = crypto_aead_encrypt(areq);
	}
	else {
		err = crypto_aead_decrypt(areq);
	}	

out:
	crypto_free_aead(tfm);
	aead_request_free(areq);

	return err;
}

static int rtk_gcm_crypt(struct aead_request *req, unsigned long mode)
{
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	unsigned int authsize = aead->authsize;
	u8 auth_tag[AES_BLOCK_SIZE] = {0};
	u32 cryptlen;

	ctx->cryp = crypto;

	if (!ctx->cryp) {
		DBG_CRYPTO_ERR("Get rtk_cryp device ERROR\n");
		return -ENODEV;
	}

	if (!req->dst) {
		DBG_CRYPTO_ERR("req dst is NULL\n");
		return -EINVAL;
	}

	// HW cannot handle the cases where plaintext and AAD are both 0 and only ciphertext is 0
	if ((mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - authsize;
		if (!cryptlen || req->assoclen > 1008) {  //a2eo max 1008
			return generic_gcm(req, ctx, FLG_DECRYPT);
		}
	}
	else {
		cryptlen = req->cryptlen;
		if (cryptlen + req->assoclen == 0) {

			memcpy(ctx->iv, req->iv, GCM_AES_IV_SIZE);
			memcpy(&(ctx->iv[12]), gcm_iv_tail, 4);

			aes_encrypt(&ctx->actx, auth_tag, ctx->iv);

			scatterwalk_map_and_copy(auth_tag, req->dst, 0, authsize, 1);

			return 0;
		}
		else if (!cryptlen || req->assoclen > 1008) {
			return generic_gcm(req, ctx, FLG_ENCRYPT);
		}	
	}

	rctx->mode = mode;

	return crypto_transfer_aead_request_to_engine(ctx->cryp->engine, req);
}

static int rtk_gcm_encrypt(struct aead_request *req)
{
	return rtk_gcm_crypt(req, FLG_GCM | FLG_ENCRYPT);
}

static int rtk_gcm_decrypt(struct aead_request *req)
{
	return rtk_gcm_crypt(req, FLG_GCM);
}

static int rtk_gcm_setauthsize(struct crypto_aead *aead, unsigned int authsize)
{
	return crypto_gcm_check_authsize(authsize);
}

static struct aead_engine_alg aes_gcm_algs[] = {
	{
		.base.base = {
			.cra_name = "gcm(aes)",
			.cra_driver_name = "rtk-gcm-aes",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = GCM_AES_IV_SIZE,
		.base.maxauthsize = AES_BLOCK_SIZE,
		.base.setauthsize = rtk_gcm_setauthsize,
		.base.setkey = rtk_gcm_setkey,
		.base.encrypt = rtk_gcm_encrypt,
		.base.decrypt = rtk_gcm_decrypt,
		.base.init = rtk_init_gcm_tfm,
		.base.exit = rtk_exit_gcm_tfm,
		.op = {
			.do_one_request = rtk_gcm_cipher_one_req,
		},		
	},
};

int rtk_gcm_alg_register(void)
{
	int ret = 0;

	ret = crypto_engine_register_aeads(aes_gcm_algs, ARRAY_SIZE(aes_gcm_algs));
	if (ret)
		goto fail;

	return 0;

fail:
	crypto_engine_unregister_aeads(aes_gcm_algs, ARRAY_SIZE(aes_gcm_algs));

	return ret;
}
