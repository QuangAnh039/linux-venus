#ifndef __RTK_CRYPTO_BASE_CTRL_H__
#define __RTK_CRYPTO_BASE_CTRL_H__


/* Data Struct for crypto Source descriptor */
/**
 *  \brief Define data struct for hp crypto Source descriptor.
 */
typedef union {
    /**
     *  \brief Source crypto descriptor data format 1.
     */
    struct {
        uint32_t key_len:4;         //!<  [3..0] Key length
        uint32_t iv_len:4;          //!<  [7..4] Initial vector length
#ifdef CONFIG_CRYPTO_DEV_RTK_MERCURY
	uint32_t keypad_len:6;      //!<  [13..8] HMAC key padding length
	uint32_t ok:1;              //!<  [14..14] ok=0: normal mode; ok=1 use OTP key
	uint32_t pk:1;              //!<  [15..15] pk=0: normal mode; pk=1 use secure keypad
	uint32_t hash_iv_len:5;     //!<  [20..16] Hash initial value length
	uint32_t nd:1;              //!<  [21..21] No destion description is required
#else
        uint32_t keypad_len:7;      //!<  [14..8] HMAC key padding length
        uint32_t pk:1;              //!<  [15..16] pk=0: normal mode; pk=1 use secure keypad
        uint32_t hash_iv_len:6;     //!<  [21..16] Hash initial value length
#endif
        /**
          *     - 2'b00: disable auto-padding
          *     - 2'b01: auto-padding for MD5/SHA1/SHA2-224/SHA2-256
          */
        uint32_t ap:2;              //!<  [23..22] Auto-padding
        uint32_t cl:2;              //!<  [25..24] Command(setting registers length). When set CL=3, it indicates that this descriptor is pointing to the command setting buffer.
        //uint32_t rsvd1:2;           //!<  [27..26] Reserve 
        uint32_t sk:1;              //!<  [26..26] sk=0: normal mode; sk=1 use secure key
        uint32_t wk:1;              //!<  [27..27] wk=0: disable; wk=1 enable, write back the secure key to the key storage

        /**
          * For descriptor which prepares essential parameters, when set, it indicates that this is the last descriptor of an IP packet.
          */
        uint32_t ls:1;              //!<  [28..28] Last segment descriptor.

        /**
          * For descriptor which prepares essential parameters, when set this bit with CL=3,
          * it indicates that this is the first descriptor of an IP packet. Except setting message buffer address to descriptor, 
          * other setting situations(Key Array, IV Array, PAD Array, SHIVL) need set this field is "1" as well.
          */
        uint32_t fs:1;              //!<  [29..29] First segment descriptor. 
        uint32_t rs:1;              //!<  [30..30] Read data from Non-Secure/Secure master port: 1=Secure port, 0=Non-secure port
        uint32_t rsvd2:1;           //!<  [31..31] Reserve
    } b;

    /**
     *  \brief Source crypto descriptor data format 2.
     */
    struct {
        uint32_t apl:8;             //!<  [7..0] Authentication padding length
        uint32_t a2eo:5;            //!<  [12..8] Set Additional authentication data length/Encryption padding data length(Mix mode only).

        /**
          * - This bit is set to zero, it indicates bit field[12..8] is used to set Additional authentication data length.
          * - This bit is set to one, it indicates bit field[12..8] is used to set Encryption padding data length(Mix mode only).
          */
        uint32_t zero:1;            //!<  [13..13] Indicates bit fields[12..8] is used to set A2EO or EPL.
        uint32_t enl:14;            //!<  [27..14] Encryption length(The message buffer length of Hash/Cipher handling).

        /**
          * For descriptor which prepares data, when set, it indicates that this is the last descriptor of an IP packet.
          */
        uint32_t ls:1;              //!<  [28..28] Last segment descriptor.

        /**
          * For descriptor which prepares data, when this field is set "0", 
          * it indicates that this descriptor is used to set message buffer.
          */
        uint32_t fs:1;              //!<  [29..29] First segment descriptor.
        uint32_t rs:1;              //!<  [30..30] Read data from Non-Secure/Secure master port: 1=Secure port, 0=Non-secure port
        uint32_t rsvd:1;            //!<  [31..31] Reserve
    } d;

    uint32_t w;
} rtl_crypto_srcdesc_t;

/* Data Struct for crypto destination descriptor */
/**
 *  \brief Define data struct for hp crypto destination descriptor.
 */
typedef union {
    /**
     *  \brief Destination crypto descriptor data format 1.To get authentication digest after crypto engine process.
     */
    struct {
        /**
         * The length of digest which uses 1 byte as a unit for Hash algorithms and Tag value in cipher algorithms.
         */
        uint32_t adl:8;             //!<  [7..0] Authentication Data length
        uint32_t rsvd1:19;          //!<  [26..8] Reserve
        /**
         * For descriptor which sets auth digest,set this field is "0",it indicates this destination descriptor is set for authentication.
         */
        uint32_t enc:1;             //!<  [27..27] Flag of encryption: 1=Cipher, 0=Hash(authentication)
        uint32_t ls:1;              //!<  [28..28] Last segment descriptor. When set, indicates that this is the last descriptor of an IP packket
        uint32_t fs:1;              //!<  [29..29] First segment descriptor. When set, indicates that this is the first descriptor of an IP packket
	uint32_t ws:1;              //!<  [30..30] Write data from Non-Secure/Secure master port:
					// 1=Secure port, 0=Non-secure port
        uint32_t rsvd2:1;           //!<  [31..31] Reserve
    } auth;

    /**
     *  \brief Destination crypto descriptor data format 2.To get cipher result after crypto engine process.
     */
    struct {
        /**
         * The length of cipher result which uses 1 byte as a unit.
         */
        uint32_t enl:24;            //!<  [23..0] Encryption length(The result length of Cipher handling)
        uint32_t rsvd1:3;           //!<  [26..24] Reserve
        /**
         * For descriptor which sets cipher result,set this field is "1",it indicates this destination descriptor is set for cipher.
         */
        uint32_t enc:1;             //!<  [27..27] Flag of encryption: 1=Cipher, 0=Hash(authentication)
        uint32_t ls:1;              //!<  [28..28] Last segment descriptor. When set, indicates that this is the last descriptor of an IP packket
        uint32_t fs:1;              //!<  [29..29] First segment descriptor. When set, indicates that this is the first descriptor of an IP packket
        uint32_t ws:1;              //!<  [30..30] Write data from Non-Secure/Secure master port: 1=Secure port, 0=Non-secure port
        uint32_t rsvd2:1;           //!<  [31..31] Reserve
    } cipher;

    uint32_t w;

} rtl_crypto_dstdesc_t;

/* Data Struct for crypto source descriptor(Command setting) */
/**
 *  \brief Source descriptor command setting data format.
 */
typedef struct rtl_crypto_cl_struct_s {
    // offset 0
    /**
      *     - AES engine:
      *         - 4'd0: ECB
      *         - 4'd1: CBC
      *         - 4'd2: CFB
      *         - 4'd3: OFB
      *         - 4'd4: CTR
      *         - 4'd5: GCTR
      *         - 4'd6: GMAC
      *         - 4'd7: GHASH
      *         - 4'd8: GCM
      *         - 4'd9: AES_CTR32
      */
    u32 cipher_mode:4;              //!<  Command setting offset 0x0: [3..0] Cipher mode

    /**
      *     - 2'd0: AES
      */
    u32 cipher_eng_sel:2;           //!<  [5..4] Cipher engine select
    u32 rsvd1:1;                    //!<  [6..6] Reserve
    u32 cipher_encrypt:1;           //!<  [7..7] Flag of cipher encryption: 1=Cipher encrypt, 0=Cipher decrypt

    /**
      *     - 2'd0: 128-bits key
      *     - 2'd1: 192-bits key
      *     - 2'd2: 256-bits key
      */    
    u32 aes_key_sel:2;              //!<  [9..8] AES key type select
    //u32 rsvd2:1;                    //!<  [10..10] Reserve
    //u32 rsvd3:1;                    //!<  [11..11] Reserve
    u32 des3_en:1;                  //!<  [10..10] Flag of 3DES enable: 1=3DES enable, 0=3DES disable
    /**
      *     - 1'd0: enc->dec->enc; dec->enc->dec
      *     - 1'd1: enc->enc->enc; dec->dec->dec
      */
    u32 des3_type:1;                //!<  [11..11] 3DES type
    u32 ckbs:1;                     //!<  [12..12] Cipher key byte swap 1=Enable, 0=Disable
    u32 hmac_en:1;                  //!<  [13..13] Flag of HMAC enable: 1=Enable, 0=Disable

    /**
      *     - 3'd0: MD5
      *     - 3'd1: SHA1
      *     - 3'd2: SHA2_224
      *     - 3'd3: SHA2_256
      *     - 3'd4: SHA2_384
      *     - 3'd5: SHA2_512
      */
    u32 hmac_mode:3;                //!<  [16..14] HMAC mode

    /**
      * 1=This message buffer is sequential hash last one, 0=This message buffer isn't sequential hash last one
      */
    u32 hmac_seq_hash_last:1;       //!<  [17..17] Sequential hash last one state:

    /**
      *     - 3'd0: Cipher only
      *     - 3'd1: Hash only
      *     - 3'd2: SSH-enc/ESP-dec
      *     - 3'd3: SSH-dec/ESP-enc
      *     - 3'd4: SSL/TLS-enc
      */
    u32 engine_mode:3;              //!<  [20..18] Engine mode
    
    /**
      * 1=This message buffer is sequential hash first one, 0=This message buffer isn't sequential hash first one
      */
    u32 hmac_seq_hash_first:1;      //!<  [21..21] Sequential hash first one state:
    u32 hmac_seq_hash:1;            //!<  [22..22] Sequential hash mechanism enable: 1=Enable, 0=Disable
    u32 hmac_seq_hash_no_wb:1;      //!<  [23..23] Sequential hash no write back: 1=no write back, 0=wirte back
    u32 icv_total_length:8;         //!<  [31..24] Initial Check Vector Total Length(Hash digest length, less than/equal to/Maximum)

    // offset 4
    u32 aad_last_data_size:4;       //!<  Command setting offset 0x4: [3..0] AAD last data size
    u32 enc_last_data_size:4;       //!<  [7..4] Encryption last data size
    u32 pad_last_data_size:3;       //!<  [10..8] Mix mode: Hash padding last data size
    u32 ckws:1;                     //!<  [11..11] Cipher key word swap: 1=Enable, 0=Disable
    u32 enc_pad_last_data_size:3;   //!<  [14..12] Mix mode: Encryption padding last data size
    u32 hsibs:1;                    //!<  [15..15] Hash sequential initial value byte swap: 1=Enable, 0=Disable
    u32 caws:1;                     //!<  [16..16] Cipher aligned word swap: 1=Enable, 0=Disable
    u32 cabs:1;                     //!<  [17..17] Cipher aligned byte swap: 1=Enable, 0=Disable
    u32 ciws:1;                     //!<  [18..18] Cipher input word swap 1=Enable, 0=Disable
    u32 cibs:1;                     //!<  [19..19] Cipher input byte swap: 1=Enable, 0=Disable
    u32 cows:1;                     //!<  [20..20] Cipher output word swap: 1=Enable, 0=Disable
    u32 cobs:1;                     //!<  [21..21] Cipher output byte swap: 1=Enable, 0=Disable
    u32 codws:1;                    //!<  [22..22] Cipher output double word swap: 1=Enable, 0=Disable
    u32 cidws:1;                    //!<  [23..23] Cipher input double word swap: 1=Enable, 0=Disable
    u32 haws:1;                     //!<  [24..24] Hash aligned word swap: 1=Enable, 0=Disable
    u32 habs:1;                     //!<  [25..25] Hash aligned byte swap: 1=Enable, 0=Disable
    u32 hiws:1;                     //!<  [26..26] Hash input word swap: 1=Enable, 0=Disable
    u32 hibs:1;                     //!<  [27..27] Hash input byte swap: 1=Enable, 0=Disable
    u32 hows:1;                     //!<  [28..28] Hash output word swap: 1=Enable, 0=Disable
    u32 hobs:1;                     //!<  [29..29] Hash output byte swap: 1=Enable, 0=Disable
    u32 hkws:1;                     //!<  [30..30] Hash key word swap: 1=Enable, 0=Disable
    u32 hkbs:1;                     //!<  [31..31] Hash key byte swap: 1=Enable, 0=Disable

    // offset 8
    u32 hash_pad_len:8;             //!<  Command setting offset 0x8: [7..0] Mix mode: Hash padding total length

    /**
     * This is the total length of AAD data. For AES_GCM/AES_GMAC and Mix_mode, use 16bytes or 8bytes as a unit.
     */
    u32 header_total_len:6;         //!<  [13..8] Header total length(AAD total length):
    u32 eptl:2;                     //!<  [15..14] Mix mode: Encryption padding total length

    /**
     * This is the total length of message buffer that crypto engine can calculate. 
     * A unit for different cryptographic features has different length meanings. 
     * The details are listed below:
     * - Hash (MD5/SHA1/SHA2-224/SHA2-256): 64 bytes as a unit
     * - Hash (MD5/SHA1/SHA2-224/SHA2-256) Auto Padding: 16 bytes as a unit
     * - Cipher (AES): 16 bytes as a unit
     * - Cipher (ESP/SSH/SSL-TLS): 16 bytes as a unit
     */
    u32 enl:16;                     //!<  [31..16] Encryption total length(Cipher/Hash)
#ifdef CONFIG_CRYPTO_DEV_RTK_MERCURY
    // offset 0xC
	u32 reserved;                   //!<   Mercury requirement
#endif
    // offset
    u32 ap0;                        //!<  Command setting offset 0xC: [31..0] padding information0(all message length)
    u32 ap1;                        //!<  Command setting offset 0x10: [31..0] padding information1(all message length)
    u32 ap2;                        //!<  Command setting offset 0x14: [31..0] padding information2(all message length)
    u32 ap3;                        //!<  Command setting offset 0x18: [31..0] padding information3(all message length)
} rtl_crypto_cl_t;

#endif
