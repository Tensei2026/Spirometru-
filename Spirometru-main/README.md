# Spirometru Electronic Portabil
## Ghid complet de instalare și utilizare

---

## Structura proiectului

```
spirometru/
├── firmware/
│   └── spirometru.ino       ← cod ESP32-C3
└── pwa/
    ├── index.html           ← aplicatia web
    ├── style.css
    ├── app.js
    ├── manifest.json
    ├── sw.js
    ├── qr-spirometru.png    ← QR de imprimat pe dispozitiv
    └── icons/
        ├── icon-192.png
        └── icon-512.png
```

---

## PASUL 1 — Firmware ESP32-C3

### Librarii necesare in Arduino IDE:
- `ESP32 BLE Arduino` (vine cu pachetul ESP32)
- `Adafruit SSD1306` (instalat din Library Manager)
- `Adafruit GFX Library` (dependinta SSD1306)

### Setari Arduino IDE:
- **Board:** ESP32C3 Dev Module
- **Flash Mode:** QIO
- **Upload Speed:** 921600
- **USB CDC On Boot:** Enabled (pentru Serial monitor)

### Pinii utilizati (conform schemei electrice):
| GPIO | Functie |
|------|---------|
| 0    | Buton multifunctional |
| 1    | ADC senzor (ESP_ADC) |
| 3    | ADC baterie (BAT_ADC) |
| 4    | Activare masurare baterie (PWR_V_METER) |
| 6    | SDA display OLED |
| 7    | SCL display OLED |
| 21   | Activare convertor boost (PWR_STEP_UP) |

### Utilizare buton:
- **Apasare scurta** — Start/Stop masurare manuala
- **Apasare lunga (2s)** — Recalibrare offset (fara flux de aer!)

---

## PASUL 2 — Hosting aplicatie web (GRATUIT)

### Varianta recomandata: GitHub Pages

1. Creeaza cont gratuit pe https://github.com
2. Creeaza repository nou numit `spirometru`
3. Incarca toate fisierele din folderul `pwa/` in repository
4. Mergi la Settings → Pages → Branch: main → Save
5. Site-ul tau va fi la: `https://NUMELE_TAU.github.io/spirometru`

### Actualizeaza QR-ul:
Dupa ce ai URL-ul final, ruleaza din nou scriptul Python din README
sau editeaza manual URL-ul in fisierul `app.js` (linia cu `BLE_SERVICE`
nu se schimba, doar URL-ul din QR).

**Sau imprim QR-ul existent** si scrie URL-ul manual pe dispozitiv
pana hostuiesti site-ul.

---

## PASUL 3 — Instalare pe telefon

1. Deschide Chrome pe telefon Android
2. Mergi la URL-ul site-ului
3. Chrome va afisa banner "Adauga pe ecranul principal"
4. Apasa "Adauga" — aplicatia apare ca o aplicatie normala
5. **SAU** scanezi QR-ul de pe dispozitiv → se deschide Chrome → instalezi

---

## PASUL 4 — Utilizare

### Adaugare pacient:
1. Apasa `+ Pacient nou`
2. Completeaza: Nume, Varsta, Sex, Inaltime (obligatorii)
3. Salveaza

### Efectuarea masuratorii:
1. Porneste spirometrul (LED-ul de pe ESP32 se aprinde)
2. Deschide aplicatia pe telefon
3. Apasa **Conecteaza** — telefonul cauta automat "Spirometru"
4. Selecteaza pacientul din lista
5. Tab **Masurare** → apasa **Start masurare**
6. Pacientul expira fortat in spirometru
7. Apasa **Stop** — valorile PEF si FEV1 apar pe ecran
8. Apasa **Salveaza rezultatul**

### Vizualizare rezultate:
- **Tab Istoric** — toate masuratorile cu data si ora
- **Tab Grafic** — evolutia PEF sau FEV1 in timp
- **Zona colorata GINA:**
  - 🟢 Verde = ≥ 80% din valoarea prezisa (normal)
  - 🟡 Galben = 60-79% (obstructie moderata)
  - 🔴 Rosu = < 60% (obstructie severa)

### Stergere automata date:
Datele mai vechi de **10 zile** se sterg automat la fiecare pornire
a aplicatiei. Nu este nevoie de nicio actiune manuala.

---

## Formule de calibrare utilizate

### PEF (din datele experimentale reale, N=30, R²=0.9965):
```
PEF [L/min] = -0.00008658 × ADC² + 0.659186 × ADC - 579.461
```

### FEV1 (integrare numerica trapezoidala pe prima secunda):
```
FEV1 [L] = Σ (Q(t) / SAMPLE_RATE)   pentru t ∈ [0, 1s]
         unde Q(t) = PEF(ADC(t)) / 60  [L/s]
```

### Valori normale PEF (Nunn & Gregg, 1989):
Calcul automat in aplicatie pe baza varstei, sexului si inaltimii pacientului.

### Valori normale FEV1 (GLI-2012 simplificat):
Calcul automat in aplicatie.

---

## Depanare

| Problema | Solutie |
|---------|---------|
| Nu gaseste spirometrul | Verifica ca BLE e pornit pe telefon; ESP32 sa fie pornit |
| Web Bluetooth nu merge | Foloseste Chrome (nu Firefox, nu Safari) |
| Valorile PEF sunt 0 | Recalibreaza (apasare lunga pe buton, fara flux) |
| Aplicatia nu se instaleaza | Site-ul trebuie sa fie pe HTTPS (GitHub Pages e HTTPS automat) |
| Date disparute | Datele peste 10 zile se sterg automat — comportament normal |
