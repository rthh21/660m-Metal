# Test pregătit pentru etapa hardware — NOT COMPILED / NOT TESTED

Nu se execută în această sesiune Windows/WSL. Sursa nu instalează și nu activează
driverul. Este destinată numai unui test ulterior, după închiderea blocajelor
hardware și aprobarea separată a utilizatorului. `--list` enumeră dispozitivele;
numai `--run` trimite comenzi GPU. Enumerarea poate implica inițializarea normală
a driverului Metal de către sistem; nu este declarată observație fără efecte.

Compilare ulterioară pe macOS, cu SDK Metal/Xcode Command Line Tools:

```sh
xcrun swiftc -swift-version 5 -O -framework Metal MetalCheck.swift -o MetalCheck
./MetalCheck --list
./MetalCheck --run --registry-id 0xID_DIN_LISTA --stage compute
./MetalCheck --run --registry-id 0xID_DIN_LISTA --stage render
./MetalCheck --run --registry-id 0xID_DIN_LISTA --stage texture
```

Înlocuiește ID-ul cu registryID-ul GPU-ului Lenovo, corelat cu IORegistry.
Numele «660M» singur nu certifică identitatea PCI. Nu se selectează automat alt
GPU și nu există fallback la CPU. Păstrează stdout JSON, stderr și exit code.

- Compute: 4.099 întregi u32, rezultat CPU determinist, verificare exactă a fiecărui
  rezultat, a inputului și a 16 cuvinte de gardă; dimensiune care nu este multiplu
  de grup. Se cere command buffer completed și absența erorii.
- Render: rulează întâi compute, apoi desenează un triunghi fullscreen într-o
  textură privată RGBA8 37×23, fără ecran/drawable. Blit de readback și verificare
  CPU a fiecărui pixel; toleranță un nivel/canal pentru conversia UNORM.
- Texture: întâi compute și render, apoi upload buffer→textură privată, citire
  din fragment shader, render și readback, cu verificarea aceluiași model de pixeli.
  Nu acoperă toate formatele, DCC, mipmap, sampling filtrat sau swizzle-urile UMD.

Timeoutul de 10 secunde este pentru notificarea command buffer după commit;
nu poate anula o comandă blocată în GPU și nu limitează timpul apelurilor de
compilare/alocare sau un blocaj în driver. Succesul probei compute este separat
de succesul randării și al texturilor. Nicio probă nu certifică display/power/sleep.

API-uri verificate în documentația primară Apple:
[MTLDevice](https://developer.apple.com/documentation/metal/mtldevice),
[completion handlers](https://developer.apple.com/documentation/metal/mtlcommandbuffer/addcompletedhandler(_:)),
[command execution](https://developer.apple.com/documentation/metal/setting-up-a-command-structure),
[storage modes](https://developer.apple.com/documentation/metal/setting-resource-storage-modes).

Fișierul este sursă pregătită și revizuită offline; absența swiftc din PATH
înseamnă că sintaxa Swift/MSL și rezultatele runtime rămân NOT TESTED. Un PASS
real necesită compilare exit 0 și logul de pe GPU-ul țintă.
