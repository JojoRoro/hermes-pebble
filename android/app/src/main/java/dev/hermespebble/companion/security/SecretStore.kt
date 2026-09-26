package dev.hermespebble.companion.security

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import java.io.File
import java.io.FileOutputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class SecretStore(context: Context) {
    private val directory = File(context.filesDir, "secure_secrets")
    private val secureRandom = SecureRandom()

    data class Bundle(
        val hermesKeyReference: String,
        val accessHeaderReference: String?,
    )

    data class Secrets(
        val hermesKey: String,
        val accessHeaderValue: String?,
    )

    suspend fun writeBundle(
        hermesKey: String,
        accessHeaderValue: String?,
    ): Bundle = withStorageContext {
        validateHermesKey(hermesKey)
        accessHeaderValue?.let(::validateAccessHeaderValue)
        val hermesReference = newReference()
        val accessReference = accessHeaderValue?.let { newReference() }
        val files = mutableListOf<File>()
        try {
            writeSecret(hermesReference, hermesKey)
            files += File(directory, fileName(hermesReference))
            if (accessReference != null && accessHeaderValue != null) {
                writeSecret(accessReference, accessHeaderValue)
                files += File(directory, fileName(accessReference))
            }
            return@withStorageContext Bundle(hermesReference, accessReference)
        } catch (error: Exception) {
            files.forEach { it.delete() }
            File(directory, ".${hermesReference}.tmp").delete()
            accessReference?.let { File(directory, ".${it}.tmp").delete() }
            throw error
        }
    }

    suspend fun readBundle(
        hermesKeyReference: String,
        accessHeaderReference: String?,
    ): Secrets = withStorageContext {
        val hermesKey = readSecret(hermesKeyReference)
        val accessHeaderValue = accessHeaderReference?.let(::readSecret)
        Secrets(hermesKey, accessHeaderValue)
    }

    suspend fun readHermesKey(reference: String): String = withStorageContext {
        readSecret(reference)
    }

    suspend fun readAccessHeaderValue(reference: String): String = withStorageContext {
        readSecret(reference)
    }

    suspend fun deleteBundle(bundle: Bundle) = withStorageContext {
        secretFile(bundle.hermesKeyReference).delete()
        bundle.accessHeaderReference?.let { secretFile(it).delete() }
    }

    private suspend fun <T> withStorageContext(block: () -> T): T =
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) { block() }

    private fun writeSecret(reference: String, value: String) {
        directory.mkdirs()
        val secretKey = getOrCreateKey()
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, secretKey, secureRandom)
        if (cipher.iv.size != IV_BYTES) {
            throw SecretStoreException("Device encryption produced an invalid IV", true, null)
        }
        cipher.updateAAD(reference.toByteArray(Charsets.UTF_8))
        val ciphertext = cipher.doFinal(value.toByteArray(Charsets.UTF_8))
        val envelope = ByteArray(4 + 1 + cipher.iv.size + ciphertext.size)
        envelope[0] = ((MAGIC ushr 24) and 0xff).toByte()
        envelope[1] = ((MAGIC ushr 16) and 0xff).toByte()
        envelope[2] = ((MAGIC ushr 8) and 0xff).toByte()
        envelope[3] = (MAGIC and 0xff).toByte()
        envelope[4] = FORMAT_VERSION.toByte()
        cipher.iv.copyInto(envelope, destinationOffset = 5)
        ciphertext.copyInto(envelope, destinationOffset = 5 + cipher.iv.size)
        val target = secretFile(reference)
        val temporary = File(directory, ".${reference}.tmp")
        FileOutputStream(temporary).use { output ->
            output.write(envelope)
            output.fd.sync()
        }
        try {
            Files.move(
                temporary.toPath(),
                target.toPath(),
                StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING,
            )
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(
                temporary.toPath(),
                target.toPath(),
                StandardCopyOption.REPLACE_EXISTING,
            )
        }
    }

    private fun readSecret(reference: String): String {
        val file = try {
            secretFile(reference)
        } catch (_: Exception) {
            throw SecretStoreException("Stored credentials are unavailable; enter them again", true, null)
        }
        if (!file.isFile || file.length() !in (HEADER_SIZE + 1).toLong()..MAX_ENVELOPE_BYTES.toLong()) {
            throw SecretStoreException("Stored credentials are invalid; enter them again", true, null)
        }
        val envelope = try {
            file.readBytes()
        } catch (_: Exception) {
            throw SecretStoreException("Stored credentials are unavailable; enter them again", true, null)
        }
        if (envelope.size <= HEADER_SIZE || envelope.size > MAX_ENVELOPE_BYTES) {
            throw SecretStoreException("Stored credentials are invalid; enter them again", true, null)
        }
        val magic = ((envelope[0].toInt() and 0xff) shl 24) or
            ((envelope[1].toInt() and 0xff) shl 16) or
            ((envelope[2].toInt() and 0xff) shl 8) or
            (envelope[3].toInt() and 0xff)
        if (magic != MAGIC || envelope[4].toInt() != FORMAT_VERSION) {
            throw SecretStoreException("Stored credentials are invalid; enter them again", true, null)
        }
        val iv = envelope.copyOfRange(5, HEADER_SIZE)
        val ciphertext = envelope.copyOfRange(HEADER_SIZE, envelope.size)
        return try {
            val key = getOrCreateKey()
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG_BITS, iv))
            cipher.updateAAD(reference.toByteArray(Charsets.UTF_8))
            String(cipher.doFinal(ciphertext), Charsets.UTF_8)
        } catch (_: KeyPermanentlyInvalidatedException) {
            resetKey()
            throw SecretStoreException("The device credential key was invalidated; enter credentials again", true, null)
        } catch (_: GeneralSecurityException) {
            throw SecretStoreException("Stored credentials could not be decrypted; enter them again", true, null)
        }
    }

    @Synchronized
    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE).run {
            init(
                KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
                )
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .setRandomizedEncryptionRequired(true)
                    .build(),
            )
            generateKey()
        }
    }

    @Synchronized
    private fun resetKey() {
        runCatching {
            KeyStore.getInstance(ANDROID_KEYSTORE).apply {
                load(null)
                deleteEntry(KEY_ALIAS)
            }
        }
    }

    private fun newReference(): String = java.util.UUID.randomUUID().toString()

    private fun secretFile(reference: String): File {
        if (!REFERENCE_PATTERN.matches(reference)) {
            throw SecretStoreException("Stored credential reference is invalid", true, null)
        }
        return File(directory, fileName(reference))
    }

    private fun fileName(reference: String): String = "$reference.bin"

    companion object {
        private const val ANDROID_KEYSTORE = "AndroidKeyStore"
        private const val KEY_ALIAS = "dev.hermespebble.companion.credentials.v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val TAG_BITS = 128
        private const val IV_BYTES = 12
        private const val HEADER_SIZE = 5 + IV_BYTES
        private const val FORMAT_VERSION = 1
        private const val MAGIC = 0x48503332
        private const val MAX_ENVELOPE_BYTES = 32 * 1024
        private val REFERENCE_PATTERN = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

        fun validateHermesKey(value: String) {
            if (value.isEmpty() || value.length > MAX_HERMES_KEY_CHARS) {
                throw SecretStoreException("Hermes API key must contain 1 to 4096 characters", false, null)
            }
            if (value.any { it.code < 0x21 || it.code > 0x7e }) {
                throw SecretStoreException("Hermes API key contains an invalid character", false, null)
            }
        }

        fun validateAccessHeaderValue(value: String) {
            if (value.isEmpty() || value.length > MAX_HEADER_VALUE_CHARS) {
                throw SecretStoreException("Access header value must contain 1 to 8192 characters", false, null)
            }
            if (value != value.trim() || value.any { it.code !in 0x20..0x7e }) {
                throw SecretStoreException("Access header value contains surrounding whitespace, a control or non-ASCII character", false, null)
            }
        }
    }
}

class SecretStoreException(
    message: String,
    val requiresReentry: Boolean,
    cause: Throwable?,
) : Exception(message, cause)

private const val MAX_HERMES_KEY_CHARS = 4096
private const val MAX_HEADER_VALUE_CHARS = 8192
