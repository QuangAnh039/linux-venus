#include "rtk-core.h"

static int rtk_init_tfm(struct crypto_skcipher *tfm)
{
	//setup some information to ctx?
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);
	int ret = 0;

	crypto_skcipher_set_reqsize(tfm, sizeof(struct rtk_cryp_reqctx));

	memset(ctx, 0, sizeof(*ctx));

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
		goto err_padding;
	}
	return 0;

err_padding:
	dma_free_coherent(ctx->cryp->dev, 32, ctx->cmd_setting, ctx->p_cmd_setting);
err_cmd:
	dma_free_coherent(ctx->cryp->dev, 16, ctx->iv, ctx->p_iv);
err_iv:
	dma_free_coherent(ctx->cryp->dev, 32, ctx->key, ctx->p_key);

	return ret;
}

static void rtk_exit_tfm(struct crypto_skcipher *tfm)
{
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);
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

}

static int rtk_cryp_crypt(struct skcipher_request *req, unsigned long mode)
{
	struct crypto_skcipher *skcipher = crypto_skcipher_reqtfm(req);
	struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(skcipher);
	struct rtk_cryp_reqctx *rctx = skcipher_request_ctx(req);
	ctx->cryp = crypto;

	if (!ctx->cryp) {
		DBG_CRYPTO_ERR("Get rtk_cryp device ERROR\n");
		return -ENODEV;
	}

	rctx->mode = mode;

	if (req->cryptlen == 0)
		return 0;

	//[extra-test] need to check?
	if ((rctx->mode & CIPHER_TYPE_MASK_BLOCK) == FLG_ECB || (rctx->mode & CIPHER_TYPE_MASK_BLOCK) == FLG_CBC) {
		switch (rctx->mode & CIPHER_KEY_MODE_MASK)
		{
			case FLG_AES:
				if ((req->cryptlen % AES_BLOCK_SIZE) != 0)
					goto err_len;
				break;
			case FLG_DES:
				if ((req->cryptlen % DES_BLOCK_SIZE) != 0)
					goto err_len;
				break;
			case FLG_TDES:
				if ((req->cryptlen % DES_BLOCK_SIZE) != 0)
					goto err_len;
				break;
default:
				DBG_CRYPTO_ERR("Unknown mode, it must be AES/DES/TDES\n");
				return -EINVAL;
		}
	}

	return crypto_transfer_skcipher_request_to_engine(ctx->cryp->engine, req);

err_len:
	return -EINVAL;
}

//************************************
// AES function
//************************************

static int rtk_aes_ecb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_ECB);
}

static int rtk_aes_ecb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_ECB | FLG_ENCRYPT);
}

static int rtk_aes_cbc_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CBC);
}

static int rtk_aes_cbc_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CBC | FLG_ENCRYPT);
}

static int rtk_aes_cfb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CFB);
}

static int rtk_aes_cfb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CFB | FLG_ENCRYPT);
}

static int rtk_aes_ofb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_OFB);
}

static int rtk_aes_ofb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_OFB | FLG_ENCRYPT);
}

static int rtk_aes_ctr_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CTR);
}

static int rtk_aes_ctr_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_AES | FLG_CTR | FLG_ENCRYPT);
}

static int rtk_aes_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen)
{
	if (keylen != AES_KEYSIZE_128 && keylen != AES_KEYSIZE_192 && keylen != AES_KEYSIZE_256)
		return -EINVAL;
	else {
		struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);

		memset(ctx->key, 0, 32);
		memcpy(ctx->key, key, keylen);
		ctx->keylen = keylen;
	}
	return 0;
}

//************************************
// DES function
//************************************

static int rtk_des_ecb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_ECB);
}

static int rtk_des_ecb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_ECB | FLG_ENCRYPT);
}

static int rtk_des_cbc_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CBC);
}

static int rtk_des_cbc_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CBC | FLG_ENCRYPT);
}

static int rtk_des_cfb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CFB);
}

static int rtk_des_cfb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CFB | FLG_ENCRYPT);
}

static int rtk_des_ofb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_OFB);
}

static int rtk_des_ofb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_OFB | FLG_ENCRYPT);
}

static int rtk_des_ctr_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CTR);
}

static int rtk_des_ctr_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_DES | FLG_CTR | FLG_ENCRYPT);
}
static int rtk_des_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen)
{
	int ret = 0;

	if (keylen != DES_KEY_SIZE)
		return -EINVAL;
	else {
		ret = verify_skcipher_des_key(tfm, key);
		if (ret == 0) {
			struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);

			memset(ctx->key, 0, keylen);
			memcpy(ctx->key, key, keylen);
			ctx->keylen = keylen;
		}
		else
			return ret;
	}

	return 0;
}

//************************************
// 3DES function
//************************************

static int rtk_3des_ecb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_ECB);
}

static int rtk_3des_ecb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_ECB | FLG_ENCRYPT);
}

static int rtk_3des_cbc_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CBC);
}

static int rtk_3des_cbc_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CBC | FLG_ENCRYPT);
}

static int rtk_3des_cfb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CFB);
}

static int rtk_3des_cfb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CFB | FLG_ENCRYPT);
}

static int rtk_3des_ofb_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_OFB);
}

static int rtk_3des_ofb_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_OFB | FLG_ENCRYPT);
}

static int rtk_3des_ctr_decrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CTR);
}

static int rtk_3des_ctr_encrypt(struct skcipher_request *req)
{
	return rtk_cryp_crypt(req, FLG_TDES | FLG_CTR | FLG_ENCRYPT);
}
static int rtk_3des_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int keylen)
{
	int ret = 0;

	if (keylen != DES3_EDE_KEY_SIZE)
		return -EINVAL;
	else {
		ret = verify_skcipher_des3_key(tfm, key);
		if (ret == 0) {
			struct rtk_cryp_tfm_ctx *ctx = crypto_skcipher_ctx(tfm);

			memset(ctx->key, 0, keylen);
			memcpy(ctx->key, key, keylen);
			ctx->keylen = keylen;
		}
		else
			return ret;
	}

	return 0;
}

static struct skcipher_engine_alg crypto_algs[] = {
/* AES - ECB */
	{
		.base.base = {
			.cra_name = "ecb(aes)",
			.cra_driver_name = "rtk-ecb-aes",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = AES_MIN_KEY_SIZE,
		.base.max_keysize = AES_MAX_KEY_SIZE,
		.base.setkey = rtk_aes_setkey,
		.base.encrypt = rtk_aes_ecb_encrypt,
		.base.decrypt = rtk_aes_ecb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* AES - CBC */
	{
		.base.base = {
			.cra_name = "cbc(aes)",
			.cra_driver_name = "rtk-cbc-aes",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = AES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = AES_MIN_KEY_SIZE,
		.base.max_keysize = AES_MAX_KEY_SIZE,
		.base.ivsize = AES_BLOCK_SIZE,
		.base.setkey = rtk_aes_setkey,
		.base.encrypt = rtk_aes_cbc_encrypt,
		.base.decrypt = rtk_aes_cbc_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* AES - CFB */
	{
		.base.base = {
			.cra_name = "cfb(aes)",
			.cra_driver_name = "rtk-cfb-aes",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = AES_MIN_KEY_SIZE,
		.base.max_keysize = AES_MAX_KEY_SIZE,
		.base.ivsize = AES_BLOCK_SIZE,
		.base.setkey = rtk_aes_setkey,
		.base.encrypt = rtk_aes_cfb_encrypt,
		.base.decrypt = rtk_aes_cfb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* AES - OFB */
	{
		.base.base = {
			.cra_name = "ofb(aes)",
			.cra_driver_name = "rtk-ofb-aes",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = AES_MIN_KEY_SIZE,
		.base.max_keysize = AES_MAX_KEY_SIZE,
		.base.ivsize = AES_BLOCK_SIZE,
		.base.setkey = rtk_aes_setkey,
		.base.encrypt = rtk_aes_ofb_encrypt,
		.base.decrypt = rtk_aes_ofb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* AES - CTR */
	{
		.base.base = {
			.cra_name = "ctr(aes)",
			.cra_driver_name = "rtk-ctr-aes",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = AES_MIN_KEY_SIZE,
		.base.max_keysize = AES_MAX_KEY_SIZE,
		.base.ivsize = AES_BLOCK_SIZE,
		.base.setkey = rtk_aes_setkey,
		.base.encrypt = rtk_aes_ctr_encrypt,
		.base.decrypt = rtk_aes_ctr_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* DES - ECB */
	{
		.base.base = {
			.cra_name = "ecb(des)",
			.cra_driver_name = "rtk-ecb-des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES_KEY_SIZE,
		.base.max_keysize = DES_KEY_SIZE,
		.base.setkey = rtk_des_setkey,
		.base.encrypt = rtk_des_ecb_encrypt,
		.base.decrypt = rtk_des_ecb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* DES - CBC */
	{
		.base.base = {
			.cra_name = "cbc(des)",
			.cra_driver_name = "rtk-cbc-des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES_KEY_SIZE,
		.base.max_keysize = DES_KEY_SIZE,
		.base.ivsize = DES_BLOCK_SIZE,
		.base.setkey = rtk_des_setkey,
		.base.encrypt = rtk_des_cbc_encrypt,
		.base.decrypt = rtk_des_cbc_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* DES - CFB */
	{
		.base.base = {
			.cra_name = "cfb(des)",
			.cra_driver_name = "rtk-cfb-des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES_KEY_SIZE,
		.base.max_keysize = DES_KEY_SIZE,
		.base.ivsize = DES_BLOCK_SIZE,
		.base.setkey = rtk_des_setkey,
		.base.encrypt = rtk_des_cfb_encrypt,
		.base.decrypt = rtk_des_cfb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* DES - OFB */
	{
		.base.base = {
			.cra_name = "ofb(des)",
			.cra_driver_name = "rtk-ofb-des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES_KEY_SIZE,
		.base.max_keysize = DES_KEY_SIZE,
		.base.ivsize = DES_BLOCK_SIZE,
		.base.setkey = rtk_des_setkey,
		.base.encrypt = rtk_des_ofb_encrypt,
		.base.decrypt = rtk_des_ofb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* DES - CTR */
	{
		.base.base = {
			.cra_name = "ctr(des)",
			.cra_driver_name = "rtk-ctr-des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES_KEY_SIZE,
		.base.max_keysize = DES_KEY_SIZE,
		.base.ivsize = DES_BLOCK_SIZE,
		.base.setkey = rtk_des_setkey,
		.base.encrypt = rtk_des_ctr_encrypt,
		.base.decrypt = rtk_des_ctr_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* 3DES - ECB */
	{
		.base.base = {
			.cra_name = "ecb(des3_ede)",
			.cra_driver_name = "rtk-ecb-3des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES3_EDE_KEY_SIZE,
		.base.max_keysize = DES3_EDE_KEY_SIZE,
		.base.setkey = rtk_3des_setkey,
		.base.encrypt = rtk_3des_ecb_encrypt,
		.base.decrypt = rtk_3des_ecb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* 3DES - CBC */
	{
		.base.base = {
			.cra_name = "cbc(des3_ede)",
			.cra_driver_name = "rtk-cbc-3des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = DES3_EDE_BLOCK_SIZE,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES3_EDE_KEY_SIZE,
		.base.max_keysize = DES3_EDE_KEY_SIZE,
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.setkey = rtk_3des_setkey,
		.base.encrypt = rtk_3des_cbc_encrypt,
		.base.decrypt = rtk_3des_cbc_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* 3DES - CFB */
	{
		.base.base = {
			.cra_name = "cfb(des3_ede)",
			.cra_driver_name = "rtk-cfb-3des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES3_EDE_KEY_SIZE,
		.base.max_keysize = DES3_EDE_KEY_SIZE,
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.setkey = rtk_3des_setkey,
		.base.encrypt = rtk_3des_cfb_encrypt,
		.base.decrypt = rtk_3des_cfb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* 3DES - OFB */
	{
		.base.base = {
			.cra_name = "ofb(des3_ede)",
			.cra_driver_name = "rtk-ofb-3des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES3_EDE_KEY_SIZE,
		.base.max_keysize = DES3_EDE_KEY_SIZE,
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.setkey = rtk_3des_setkey,
		.base.encrypt = rtk_3des_ofb_encrypt,
		.base.decrypt = rtk_3des_ofb_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
/* 3DES - CTR */
	{
		.base.base = {
			.cra_name = "ctr(des3_ede)",
			.cra_driver_name = "rtk-ctr-3des",
			.cra_priority = 400,
			.cra_flags = CRYPTO_ALG_ASYNC,
			.cra_blocksize = 1,
			.cra_ctxsize = sizeof(struct rtk_cryp_tfm_ctx),
			.cra_module = THIS_MODULE,
		},
		.base.min_keysize = DES3_EDE_KEY_SIZE,
		.base.max_keysize = DES3_EDE_KEY_SIZE,
		.base.ivsize = DES3_EDE_BLOCK_SIZE,
		.base.setkey = rtk_3des_setkey,
		.base.encrypt = rtk_3des_ctr_encrypt,
		.base.decrypt = rtk_3des_ctr_decrypt,
		.base.init = rtk_init_tfm,
		.base.exit = rtk_exit_tfm,
		.op = {
			.do_one_request = rtk_cryp_cipher_one_req,
		},
	},
};

int rtk_crypto_alg_register(void)
{
	int ret = 0;

	ret = crypto_engine_register_skciphers(crypto_algs, ARRAY_SIZE(crypto_algs));
	if (ret)
		goto fail;

	return 0;

fail:
	crypto_engine_unregister_skciphers(crypto_algs, ARRAY_SIZE(crypto_algs));

	return ret;
}
