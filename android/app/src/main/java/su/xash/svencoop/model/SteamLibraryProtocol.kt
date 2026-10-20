package su.xash.svencoop.model

import java.io.ByteArrayOutputStream

internal object SteamLibraryProtocol {
    const val APP_ID = 225840

    private fun field(output: ByteArrayOutputStream, key: Int, value: Long) {
        fun varint(number: Long) {
            var remaining = number
            while (remaining and -128L != 0L) {
                output.write((remaining.toInt() and 127) or 128)
                remaining = remaining ushr 7
            }
            output.write(remaining.toInt())
        }
        varint((key shl 3).toLong())
        varint(value)
    }

    fun ownedGamesQuery(steamId: Long): ByteArray {
        require(steamId > 0)
        return ByteArrayOutputStream().apply {
            field(this, 1, steamId)
            field(this, 3, 1)
            field(this, 4, APP_ID.toLong())
            field(this, 5, 1)
            field(this, 6, 0)
        }.toByteArray()
    }

    private class Reader(private val data: ByteArray) {
        private var position = 0
        val remaining get() = data.size - position

        fun varint(): Long {
            var result = 0L
            for (shift in 0..63 step 7) {
                require(remaining > 0) { "Truncated Steam library response" }
                val value = data[position++].toInt() and 255
                require(shift != 63 || value <= 1) { "Invalid Steam library response" }
                result = result or ((value and 127).toLong() shl shift)
                if (value and 128 == 0) return result
            }
            error("Invalid Steam library response")
        }

        fun bytes(): ByteArray {
            val length = varint()
            require(length in 0..remaining.toLong()) { "Truncated Steam library response" }
            val result = data.copyOfRange(position, position + length.toInt())
            position += length.toInt()
            return result
        }

        fun skip(wire: Int) {
            when (wire) {
                0 -> varint()
                1 -> { require(remaining >= 8); position += 8 }
                2 -> bytes()
                5 -> { require(remaining >= 4); position += 4 }
                else -> error("Invalid Steam library response")
            }
        }
    }

    fun ownsSven(response: ByteArray): Boolean {
        val reader = Reader(response)
        var owned = false
        while (reader.remaining > 0) {
            val tag = reader.varint()
            require(tag > 0 && tag <= Int.MAX_VALUE)
            val wire = tag.toInt() and 7
            if (tag.toInt() ushr 3 == 2) {
                require(wire == 2)
                val game = Reader(reader.bytes())
                var appId = 0L
                while (game.remaining > 0) {
                    val gameTag = game.varint()
                    require(gameTag > 0 && gameTag <= Int.MAX_VALUE)
                    if (gameTag.toInt() ushr 3 == 1) {
                        require(gameTag.toInt() and 7 == 0)
                        appId = game.varint()
                    } else game.skip(gameTag.toInt() and 7)
                }
                if (appId == APP_ID.toLong()) owned = true
            } else reader.skip(wire)
        }
        return owned
    }
}
