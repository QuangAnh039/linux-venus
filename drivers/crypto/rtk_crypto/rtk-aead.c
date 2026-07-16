#include "rtk-core.h"

int rtk_aead_prepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp; // need to check?
	CRYPTO_Type *base = NULL;
	int nr_sgs;
	int nr_sgd;
	struct scatterlist *cipher_src, *cipher_dst;
	int ret;
	u32 cryptlen;
	u32 authsize = aead->authsize;

	if (!cryp) {
		DBG_CRYPTO_ERR("Prepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	base = cryp->base_addr;

	if (!req->src || !req->dst) {
		DBG_CRYPTO_ERR("Prepare cipher req: src or dst is NULL\n");
		return -EINVAL;
	}

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		cryptlen = req->cryptlen - authsize;
	}
	else {
		cryptlen = req->cryptlen;
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

			if (ret)
				return ret;
		}

		cipher_dst = scatterwalk_ffwd(rctx->cipher_dst, req->dst, req->assoclen);

		rctx->crypto_dst = cipher_dst;

		nr_sgs = dma_map_sg(cryp->dev, cipher_src, sg_nents_for_len(cipher_src, cryptlen), DMA_TO_DEVICE);
		if (nr_sgs <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid src sg number %d\n", nr_sgs);
			return -EINVAL;
		}
		ctx->cipher_nr_sgs = nr_sgs;

		nr_sgd = dma_map_sg(cryp->dev, cipher_dst, sg_nents_for_len(cipher_dst, cryptlen), DMA_FROM_DEVICE);
		if (nr_sgd <= 0) {
			DBG_CRYPTO_ERR("Prepare cipher req: Invalid dst sg number %d\n", nr_sgd);
			dma_unmap_sg(cryp->dev, cipher_src, sg_nents_for_len(cipher_src, cryptlen), DMA_TO_DEVICE);
			return -EINVAL;
		}

		ctx->cipher_nr_sgd = nr_sgd;
	}

	//store req in cryp
	cryp->cipher_req = NULL;
	cryp->hash_req = NULL;
	cryp->aead_req = req;

	//prepare iv array
	if (req->iv) {
		ctx->ivlen = crypto_aead_ivsize(aead);
		memcpy(ctx->iv, req->iv, ctx->ivlen);
	}

	return 0;
}

int rtk_aead_unprepare_cipher_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp;

	if (!cryp) {
		DBG_CRYPTO_ERR("Unprepare cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	//unmap dma
	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		dma_unmap_sg(cryp->dev, req->src, sg_nents(req->src), DMA_TO_DEVICE);
	}
	else {
		dma_unmap_sg(cryp->dev, req->dst, sg_nents(req->dst), DMA_TO_DEVICE);

		scatterwalk_map_and_copy(ctx->digest_result, req->dst, req->assoclen + req->cryptlen, aead->authsize, 1);
	}

	if (req->src == req->dst) {
		if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
			dma_unmap_sg(cryp->dev, rctx->crypto_src, sg_nents(rctx->crypto_src), DMA_BIDIRECTIONAL);
		}
	}
	else {
		if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
			dma_unmap_sg(cryp->dev, rctx->crypto_src, sg_nents(rctx->crypto_src), DMA_TO_DEVICE);
			dma_unmap_sg(cryp->dev, rctx->crypto_dst, sg_nents(rctx->crypto_dst), DMA_FROM_DEVICE);
		}
	}

	return 0;
}

static int rtk_aead_cipher_set_srcdesc(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base, u32 authsize)
{
	rtl_crypto_srcdesc_t  src_cipher_desc;
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct scatterlist *sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen;

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

static int rtk_aead_cipher_set_dstdesc(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base, u32 authsize)
{
	rtl_crypto_dstdesc_t  dst_cipher_desc;
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct scatterlist *sg;
	struct scatterlist *dst_sg;
	int i, ret;
	u32 len;
	dma_addr_t addr;
	u32 cryptlen;

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
			}

			ret = set_dst_desc(base, dst_cipher_desc.w, addr);
			if (ret)
				return ret;
		}
	}

set_dst_done:

	return 0;
}

static int rtk_cipher_setup_key_iv_cmd(struct aead_request *req, struct rtk_aead_tfm_ctx *ctx, CRYPTO_Type *base, u32 authsize)
{
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	rtl_crypto_srcdesc_t src_desc;
	int ret;
	u32 cryptlen;
	rtl_crypto_cl_t *cmd_ptr;

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
			cmd_ptr->enl = (cryptlen + 15)/AES_BLOCK_SIZE;
			cmd_ptr->enc_last_data_size = cryptlen % AES_BLOCK_SIZE;
			ctx->cryp_padding_len = (AES_BLOCK_SIZE - (cryptlen % AES_BLOCK_SIZE)) % AES_BLOCK_SIZE;

			break;
		case FLG_TDES:
			cmd_ptr->des3_en = 1;
			fallthrough;
		case FLG_DES:
			cmd_ptr->cipher_eng_sel = 1;
			//DES take 8bytes as a block to process data
			cmd_ptr->enl = (cryptlen + 7)/DES_BLOCK_SIZE;
			cmd_ptr->enc_last_data_size = cryptlen % DES_BLOCK_SIZE;
			ctx->cryp_padding_len = (DES_BLOCK_SIZE - (cryptlen % DES_BLOCK_SIZE)) % DES_BLOCK_SIZE;
			cmd_ptr->cipher_mode = rctx->mode & CIPHER_TYPE_MASK_BLOCK;
			break;
		default:
			DBG_CRYPTO_ERR("Prepare cipher req: ALG_MODE unknown, it must be AES/DES/TDES\n");
	}

	cmd_ptr->icv_total_length = 0x40; // for mix mode, but need to set a value 0x40

	wmb();

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

static int rtk_aead_hash_set_desc(struct aead_request *req, u32 sg_len, u32 authsize)
{
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	CRYPTO_Type *base = NULL;
	rtl_crypto_srcdesc_t src_desc;
	rtl_crypto_dstdesc_t dst_desc;
	struct rtk_cryp *cryp = ctx->cryp;
	rtl_crypto_cl_t *cmd_ptr;
	int ret;
	struct scatterlist *src_sg;
	struct scatterlist *sg;
	int nr_sgs, i;
	u32 len;
	dma_addr_t addr;

	base = cryp->base_addr;

	dst_desc.w = 0;
	dst_desc.auth.ws  = 1;
	dst_desc.auth.fs = 1;
	dst_desc.auth.ls = 1;
	dst_desc.auth.adl = authsize;

	ret = set_dst_desc(base, dst_desc.w, ctx->p_digest_result);

	if (ret) {
		DBG_CRYPTO_ERR("set dst desc fail\n");			
		return ret;
	}

	//clear command ok & error interrupts
	base->ipscsr_err_stats_reg = 0xFFFF;
	base->ipscsr_reset_isr_conf_reg_b.cmd_ok = 1;

	//prepare command setting buffer
	cmd_ptr = (rtl_crypto_cl_t *)ctx->cmd_setting;
	memset((u8*)cmd_ptr, 0, 32);

	cmd_ptr->engine_mode = 1; //hash only
	cmd_ptr->hmac_seq_hash = 1;

	cmd_ptr->hmac_seq_hash_first = 1;

	ctx->total_len = ctx->total_len + sg_len;
	cmd_ptr->hmac_seq_hash_last = 1;
	cmd_ptr->hmac_seq_hash_no_wb = 0;
	cmd_ptr->enc_last_data_size = ctx->total_len % 16; // pshuang need to check?
	ctx->hash_padding_len = (16 - cmd_ptr->enc_last_data_size) % 16;
	cmd_ptr->ap0 = ctx->total_len * 8;
	cmd_ptr->enl = (sg_len + 15)/16;

	cmd_ptr->habs = 1;
	switch (rctx->mode & AEAD_HASH_TYPE_MASK_BLOCK) 
	{
		case FLG_AEAD_MD5:
			cmd_ptr->hibs = 1;
			cmd_ptr->hobs = 1;
			cmd_ptr->hkbs = 1;
			break;		
		case FLG_AEAD_SHA1:
			cmd_ptr->hmac_mode = 1;
			break;
		case FLG_AEAD_SHA2_224:
			cmd_ptr->hmac_mode = 2;
			break;
		case FLG_AEAD_SHA2_256:
			cmd_ptr->hmac_mode = 3;
			break;
	}

	cmd_ptr->hmac_en = 1;
	cmd_ptr->ap0 = cmd_ptr->ap0 + (64 * 8);

	cmd_ptr->icv_total_length = 0x40;

	//set command setting
	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	src_desc.b.cl = 3;

	src_desc.b.ap = 0x01;

	ret = set_src_desc(base, src_desc.w, ctx->p_cmd_setting);

	if (ret) {
		DBG_CRYPTO_ERR("update: Set command setting to src fifo fail\n");
		return ret;
	}

	src_desc.w = 0;
	src_desc.b.rs = 1;
	src_desc.b.fs = 1;
	src_desc.b.keypad_len = 128/4;
	ret = set_src_desc(base, src_desc.w, ctx->p_keypad);
	if (ret) {
		DBG_CRYPTO_ERR("update: Set HMAC keypad to src fifo fail\n");
		return ret;
	}

	//set data fifo
	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_ENCRYPT) {
		src_sg = req->dst;
	}
	else {
		src_sg = req->src;
	}

	
	nr_sgs = dma_map_sg(cryp->dev, src_sg, sg_nents(src_sg), DMA_TO_DEVICE);
	if (nr_sgs <= 0) {
		DBG_CRYPTO_ERR("Invalid src sg number %d\n", nr_sgs);
		return -EINVAL;
	}

	for_each_sg(src_sg, sg, nr_sgs, i) {
		src_desc.w = 0;
		src_desc.d.rs = 1;

		len = sg->length;
		addr = sg_dma_address(sg);

		while (len > MAX_CRYPTO_SRC_LEN) {

			if (sg_len <= MAX_CRYPTO_SRC_LEN) {
				src_desc.d.enl = sg_len;
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
				if (ctx->hash_padding_len == 0) {
					src_desc.d.ls = 1;
				}
				ret = set_src_desc(base, src_desc.w, addr);
				if (ret)
					return ret;

				goto set_data_done;
			}
			else {
				if (sg_is_last(sg) && (ctx->hash_padding_len == 0)) {
					src_desc.d.ls = 1;
				}
				src_desc.d.enl = len;
				sg_len = sg_len - len;

				ret = set_src_desc(base, src_desc.w, addr);
				if (ret)
					return ret;
			}
		}
	}

set_data_done:

	if (ctx->hash_padding_len) {
		src_desc.w = 0;
		src_desc.d.rs = 1;
		src_desc.d.enl = ctx->hash_padding_len;
		src_desc.d.ls = 1;

		ret = set_src_desc(base, src_desc.w, ctx->p_cryp_padding);
		if (ret) {
			DBG_CRYPTO_ERR("update: Set data to src fifo fail\n");
			return ret;
		}
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
int rtk_aead_cipher_one_req(struct crypto_engine *engine, void *areq)
{
	struct aead_request *req = container_of(areq, struct aead_request, base);
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_cryp *cryp = ctx->cryp;
	CRYPTO_Type *base = NULL;
	int ret = 0, err = 0;
	u32 authsize = aead->authsize;
	DECLARE_CRYPTO_WAIT(wait);

	if (!cryp) {
		DBG_CRYPTO_ERR("Do cipher req: get rtk_cryp ERROR\n");
		return -ENODEV;
	}

	err = rtk_aead_prepare_cipher_req(engine, areq);
	if (err)
		goto aeadReqEnd;


	cryp->cipher_req = NULL;
	cryp->hash_req = NULL;
	cryp->aead_req = req;

	base = cryp->base_addr;
	rctx->first_phase = 1;
	rctx->wait = &wait;

	if ((rctx->mode & CIPHER_CRYPT_MASK) == FLG_ENCRYPT) {
		// step 1: Setup desitination descriptor
		ret = rtk_aead_cipher_set_dstdesc(req, ctx, base, authsize);
		if (ret) {
			DBG_CRYPTO_ERR("Do cipher req: setup desitination descriptor fail\n");
			return ret;
		}

		// step 2: Setup source descriptor
		//   step 2-1: prepare Key & IV array & command setting packet
		ret = rtk_cipher_setup_key_iv_cmd(req, ctx, base, authsize);
		if (ret)
			return ret;

		//   step 2-2: prepare Data1 ~ DataN
		ret = rtk_aead_cipher_set_srcdesc(req, ctx, base, authsize);
		if (ret)
			return ret;

		wait_for_completion(&wait.completion);
		ret = wait.err;
		if (ret)
			return 0;

		if (req->src == req->dst) {
			dma_unmap_sg(cryp->dev, rctx->crypto_src, ctx->cipher_nr_sgs, DMA_BIDIRECTIONAL);
		}
		else {
			dma_unmap_sg(cryp->dev, rctx->crypto_src, ctx->cipher_nr_sgs, DMA_TO_DEVICE);
			dma_unmap_sg(cryp->dev, rctx->crypto_dst, ctx->cipher_nr_sgd, DMA_FROM_DEVICE);
		}

		//hash part
		rtk_aead_hash_set_desc(req, req->assoclen + req->cryptlen, authsize);
	}
	else {
		//hash part
		rtk_aead_hash_set_desc(req, req->assoclen + req->cryptlen - authsize, authsize);


		wait_for_completion(&wait.completion);
		ret = wait.err;
		if (ret)
			return 0;

		// step 1: Setup desitination descriptor
		ret = rtk_aead_cipher_set_dstdesc(req, ctx, base, authsize);
		if (ret) {
			DBG_CRYPTO_ERR("Do cipher req: setup desitination descriptor fail\n");
			return ret;
		}

		// step 2: Setup source descriptor
		//   step 2-1: prepare Key & IV array & command setting packet
		ret = rtk_cipher_setup_key_iv_cmd(req, ctx, base, authsize);
		if (ret)
			return ret;

		//   step 2-2: prepare Data1 ~ DataN
		ret = rtk_aead_cipher_set_srcdesc(req, ctx, base, authsize);
		if (ret)
			return ret;
	}

aeadReqEnd:
	rtk_aead_unprepare_cipher_req(engine, areq);

	return 0;
}

static int rtk_init_aead_tfm(struct crypto_aead *tfm)
{
	//setup some information to ctx?
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

	ctx->digest_result = dma_alloc_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, &ctx->p_digest_result, GFP_KERNEL);
	if (!ctx->digest_result) {
		DBG_CRYPTO_ERR("dma_alloc_coherent digest_result fail\n");
		ret = -ENOMEM;
		goto err_digest;
	}

	ctx->keypad = dma_alloc_coherent(ctx->cryp->dev, 128, &ctx->p_keypad, GFP_KERNEL);
	if (!ctx->keypad) {
		DBG_CRYPTO_ERR("dma_alloc_coherent keypad fail\n");
		ret = -ENOMEM;
		goto err_keypad;
	}

	null = crypto_get_default_null_skcipher();
	if (IS_ERR(null)) {
		ret = PTR_ERR(null);
		goto err_null_skcipher;
	}

	ctx->null = null;
	//ctx->enginectx.op.do_one_request = rtk_aead_cipher_one_req;
	//ctx->enginectx.op.prepare_request = rtk_aead_prepare_cipher_req;
	//ctx->enginectx.op.unprepare_request = rtk_aead_unprepare_cipher_req;

	return 0;

err_null_skcipher:
	dma_free_coherent(ctx->cryp->dev, 128, ctx->keypad, ctx->p_keypad);
err_keypad:
	dma_free_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, ctx->digest_result, ctx->p_digest_result);
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

static void rtk_exit_aead_tfm(struct crypto_aead *tfm)
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
		dma_free_coherent(ctx->cryp->dev, SHA256_DIGEST_SIZE, ctx->digest_result, ctx->p_digest_result);

	if (ctx->keypad)
		dma_free_coherent(ctx->cryp->dev, 128, ctx->keypad, ctx->p_keypad);

	crypto_put_default_null_skcipher();
}

static int rtk_aead_aes_setkey(struct crypto_aead *aead, const u8 *key, unsigned int keylen)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct crypto_authenc_keys keys;
	u8 *ipad = &(ctx->keypad[0]);
	u8 *opad = &(ctx->keypad[64]);

	if (crypto_authenc_extractkeys(&keys, key, keylen) != 0) {
		memzero_explicit(&keys, sizeof(keys));
		return -EINVAL;
	}

	if (keys.enckeylen != AES_KEYSIZE_128 && keys.enckeylen != AES_KEYSIZE_192 && keys.enckeylen != AES_KEYSIZE_256)
		return -EINVAL;

	memcpy(ctx->key, keys.enckey, keys.enckeylen);
	ctx->keylen = keys.enckeylen;

	memset(ctx->keypad, 0, 128);
	ctx->keypad_len = keys.authkeylen;
	
	memcpy(ipad, keys.authkey, ctx->keypad_len);
	memcpy(opad, keys.authkey, ctx->keypad_len);
	
	return 0;
}

static int rtk_aead_des_setkey(struct crypto_aead *aead, const u8 *key, unsigned int keylen)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct crypto_authenc_keys keys;
	u8 *ipad = &(ctx->keypad[0]);
	u8 *opad = &(ctx->keypad[64]);
	int ret;

	if (crypto_authenc_extractkeys(&keys, key, keylen) != 0) {
		memzero_explicit(&keys, sizeof(keys));
		return -EINVAL;
	}

	if (keys.enckeylen != DES_KEY_SIZE)
		return -EINVAL;
	else {
		ret = verify_aead_des_key(aead, keys.enckey, keys.enckeylen);
		if (!ret) {
			memcpy(ctx->key, keys.enckey, keys.enckeylen);
			ctx->keylen = keys.enckeylen;
		}
		else
			return ret;
	}

	memset(ctx->keypad, 0, 128);
	ctx->keypad_len = keys.authkeylen;

	memcpy(ipad, keys.authkey, ctx->keypad_len);
	memcpy(opad, keys.authkey, ctx->keypad_len);

	return 0;
}

static int rtk_aead_3des_setkey(struct crypto_aead *aead, const u8 *key, unsigned int keylen)
{
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct crypto_authenc_keys keys;
	u8 *ipad = &(ctx->keypad[0]);
	u8 *opad = &(ctx->keypad[64]);
	int ret;

	if (crypto_authenc_extractkeys(&keys, key, keylen) != 0) {
		memzero_explicit(&keys, sizeof(keys));
		return -EINVAL;
	}

	if (keys.enckeylen != DES3_EDE_KEY_SIZE)
		return -EINVAL;
	else {
		ret = verify_aead_des3_key(aead, keys.enckey, keys.enckeylen);
		if (!ret) {
			memcpy(ctx->key, keys.enckey, keys.enckeylen);
			ctx->keylen = keys.enckeylen;
		}
		else
			return ret;
	}

	memset(ctx->keypad, 0, 128);
	ctx->keypad_len = keys.authkeylen;

	memcpy(ipad, keys.authkey, ctx->keypad_len);
	memcpy(opad, keys.authkey, ctx->keypad_len);

	return 0;
}

static int rtk_aead_crypt(struct aead_request *req, unsigned long mode)
{
	struct crypto_aead *aead = crypto_aead_reqtfm(req);
	struct rtk_aead_tfm_ctx *ctx = crypto_aead_ctx(aead);
	struct rtk_aead_reqctx *rctx = aead_request_ctx(req);
	ctx->cryp = crypto;

	if (!ctx->cryp) {
		DBG_CRYPTO_ERR("Get rtk_cryp device ERROR\n");
		return -ENODEV;
	}

	if ((mode & CIPHER_CRYPT_MASK) == FLG_DECRYPT) {
		if (req->cryptlen - aead->authsize == 0)
			return 0;
	}
	else {
		if (req->cryptlen == 0)
			return 0;
	}

	rctx->mode = mode;

	return crypto_transfer_aead_request_to_engine(ctx->cryp->engine, req);
}

//************************************
// AES function
//************************************

static int rtk_md5_aes_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_AES | FLG_AEAD_MD5);
}

static int rtk_md5_aes_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_AES | FLG_AEAD_MD5);
}

static int rtk_sha1_aes_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_AES | FLG_AEAD_SHA1);
}

static int rtk_sha1_aes_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_AES | FLG_AEAD_SHA1);
}

static int rtk_sha224_aes_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_AES | FLG_AEAD_SHA2_224);
}

static int rtk_sha224_aes_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_AES | FLG_AEAD_SHA2_224);
}

static int rtk_sha256_aes_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_AES | FLG_AEAD_SHA2_256);
}

static int rtk_sha256_aes_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_AES | FLG_AEAD_SHA2_256);
}

//************************************
// DES function
//************************************

static int rtk_md5_des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_DES | FLG_AEAD_MD5);
}

static int rtk_md5_des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_DES | FLG_AEAD_MD5);
}

static int rtk_sha1_des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_DES | FLG_AEAD_SHA1);
}

static int rtk_sha1_des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_DES | FLG_AEAD_SHA1);
}

static int rtk_sha224_des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_DES | FLG_AEAD_SHA2_224);
}

static int rtk_sha224_des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_DES | FLG_AEAD_SHA2_224);
}

static int rtk_sha256_des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_DES | FLG_AEAD_SHA2_256);
}

static int rtk_sha256_des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_DES | FLG_AEAD_SHA2_256);
}

//************************************
// 3DES function
//************************************

static int rtk_md5_3des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_TDES | FLG_AEAD_MD5);
}

static int rtk_md5_3des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_TDES | FLG_AEAD_MD5);
}

static int rtk_sha1_3des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_TDES | FLG_AEAD_SHA1);
}

static int rtk_sha1_3des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_TDES | FLG_AEAD_SHA1);
}

static int rtk_sha224_3des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_TDES | FLG_AEAD_SHA2_224);
}

static int rtk_sha224_3des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_TDES | FLG_AEAD_SHA2_224);
}

static int rtk_sha256_3des_cbc_encrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_ENCRYPT | FLG_TDES | FLG_AEAD_SHA2_256);
}

static int rtk_sha256_3des_cbc_decrypt(struct aead_request *req)
{
	return rtk_aead_crypt(req, FLG_CBC | FLG_TDES | FLG_AEAD_SHA2_256);
}

static struct aead_engine_alg aes_authenc_algs[] = {
/* AES - CBC - MD5*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(md5),cbc(aes))",
			.cra_driver_name = "rtk-authenc-hmac-md5-cbc-aes",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = AES_BLOCK_SIZE,
		.base.maxauthsize = MD5_DIGEST_SIZE,
		.base.setkey = rtk_aead_aes_setkey,
		.base.encrypt = rtk_md5_aes_cbc_encrypt,
		.base.decrypt = rtk_md5_aes_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* AES - CBC - SHA1*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha1),cbc(aes))",
			.cra_driver_name = "rtk-authenc-hmac-sha1-cbc-aes",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = AES_BLOCK_SIZE,
		.base.maxauthsize = SHA1_DIGEST_SIZE,
		.base.setkey = rtk_aead_aes_setkey,
		.base.encrypt = rtk_sha1_aes_cbc_encrypt,
		.base.decrypt = rtk_sha1_aes_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* AES - CBC - SHA224*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha224),cbc(aes))",
			.cra_driver_name = "rtk-authenc-hmac-sha224-cbc-aes",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = AES_BLOCK_SIZE,
		.base.maxauthsize = SHA224_DIGEST_SIZE,
		.base.setkey = rtk_aead_aes_setkey,
		.base.encrypt = rtk_sha224_aes_cbc_encrypt,
		.base.decrypt = rtk_sha224_aes_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* AES - CBC - SHA256*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha256),cbc(aes))",
			.cra_driver_name = "rtk-authenc-hmac-sha256-cbc-aes",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = AES_BLOCK_SIZE,
		.base.maxauthsize = SHA256_DIGEST_SIZE,
		.base.setkey = rtk_aead_aes_setkey,
		.base.encrypt = rtk_sha256_aes_cbc_encrypt,
		.base.decrypt = rtk_sha256_aes_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - MD5*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(md5),cbc(des))",
			.cra_driver_name = "rtk-authenc-hmac-md5-cbc-des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES_BLOCK_SIZE,
		.base.maxauthsize = MD5_DIGEST_SIZE,
		.base.setkey = rtk_aead_des_setkey,
		.base.encrypt = rtk_md5_des_cbc_encrypt,
		.base.decrypt = rtk_md5_des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - SHA1*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha1),cbc(des))",
			.cra_driver_name = "rtk-authenc-hmac-sha1-cbc-des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES_BLOCK_SIZE,
		.base.maxauthsize = SHA1_DIGEST_SIZE,
		.base.setkey = rtk_aead_des_setkey,
		.base.encrypt = rtk_sha1_des_cbc_encrypt,
		.base.decrypt = rtk_sha1_des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - SHA224*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha224),cbc(des))",
			.cra_driver_name = "rtk-authenc-hmac-sha224-cbc-des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES_BLOCK_SIZE,
		.base.maxauthsize = SHA224_DIGEST_SIZE,
		.base.setkey = rtk_aead_des_setkey,
		.base.encrypt = rtk_sha224_des_cbc_encrypt,
		.base.decrypt = rtk_sha224_des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - SHA256*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha256),cbc(des))",
			.cra_driver_name = "rtk-authenc-hmac-sha256-cbc-des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES_BLOCK_SIZE,
		.base.maxauthsize = SHA256_DIGEST_SIZE,
		.base.setkey = rtk_aead_des_setkey,
		.base.encrypt = rtk_sha256_des_cbc_encrypt,
		.base.decrypt = rtk_sha256_des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* 3DES - CBC - MD5*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(md5),cbc(des3_ede))",
			.cra_driver_name = "rtk-authenc-hmac-md5-cbc-3des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.maxauthsize = MD5_DIGEST_SIZE,
		.base.setkey = rtk_aead_3des_setkey,
		.base.encrypt = rtk_md5_3des_cbc_encrypt,
		.base.decrypt = rtk_md5_3des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* 3DES - CBC - SHA1*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha1),cbc(des3_ede))",
			.cra_driver_name = "rtk-authenc-hmac-sha1-cbc-3des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.maxauthsize = SHA1_DIGEST_SIZE,
		.base.setkey = rtk_aead_3des_setkey,
		.base.encrypt = rtk_sha1_3des_cbc_encrypt,
		.base.decrypt = rtk_sha1_3des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - SHA224*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha224),cbc(des3_ede))",
			.cra_driver_name = "rtk-authenc-hmac-sha224-cbc-3des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.maxauthsize = SHA224_DIGEST_SIZE,
		.base.setkey = rtk_aead_3des_setkey,
		.base.encrypt = rtk_sha224_3des_cbc_encrypt,
		.base.decrypt = rtk_sha224_3des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},
/* DES - CBC - SHA256*/
	{
		.base.base = {
			.cra_name = "authenc(hmac(sha256),cbc(des3_ede))",
			.cra_driver_name = "rtk-authenc-hmac-sha256-cbc-3des",
			.cra_priority = 2000,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_aead_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.maxauthsize = SHA256_DIGEST_SIZE,
		.base.setkey = rtk_aead_3des_setkey,
		.base.encrypt = rtk_sha256_3des_cbc_encrypt,
		.base.decrypt = rtk_sha256_3des_cbc_decrypt,
		.base.init = rtk_init_aead_tfm,
		.base.exit = rtk_exit_aead_tfm,
		.op = {
				.do_one_request = rtk_aead_cipher_one_req,
		},
	},

};

int rtk_aead_alg_register(void)
{
	int ret = 0;

	ret = crypto_engine_register_aeads(aes_authenc_algs, ARRAY_SIZE(aes_authenc_algs));
	if (ret)
		goto fail;

	return 0;

fail:
	crypto_engine_unregister_aeads(aes_authenc_algs, ARRAY_SIZE(aes_authenc_algs));

	return ret;
}
