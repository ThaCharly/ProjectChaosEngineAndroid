import mido

import sys


# Uso: python converter.py cancion.mid > nivel.txt


mid = mido.MidiFile('pvz.mid')


print("SONG_START")

for msg in mid:

    # Filtramos solo cuando empieza una nota y si no es silencio (vel > 0)

    if msg.type == 'note_on' and msg.velocity > 0:

        # msg.note es un entero (ej: 60 para Do4)

        print(msg.note)

print("SONG_END") 