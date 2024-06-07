Roadmap

## Entrées

- liste de fichiers: `Glist *` -> gtk_file_chooser()
  - Pour chaque fichier :
    - chemin (répertoire/nom de fichier),
    - EXIF
- Import simple (BdD) ou Import avec copie : `gboolean`
- Cas avec copie :
  - dossier cible,
  - pattern de nommage du sous-dossier cible : `str` (avec métacaractères)
  - pattern de nommage des fichiers : `str` (avec métacaractères)
  - Jobcode : `str`
- Optionnellement : XMPs pré-existant,
- Date des fichiers (pour patterns)


## Sorties

- Cas avec copie -> fichier copié: `I/O`
- Tous les cas -> nouvelles entrées en BdD : 
  - filmroll : ajouter l'image, `SQL`
  - image : ajouter nom, date import, EXIFs, tags etc., `SQL`
- Dans tous les cas -> mettre à jour la vue en table lumineuse (en fonction du filmroll, tags, EXIFs, etc.), `GUI`
- Optionnellement : XMPs sur disque `I/O`

## Process

- [x] [GUI] Récupérer la liste des fichiers (en fait, leurs chemins) depuis `gtk_file_chooser`
    - depuis `import.c:_selection_changed()`,
    - vers la structure `GList * ((dt_import_t *import)->files)`
- [x] [GUI] Récupérer les `str` du chemin cible, patterns sous-dossier cible, et jobcode :
    - depuis `gtk_label` 
    - vers `anselrc` config keys :
      - `session/base_directory_pattern`,
      - `session/sub_directory_pattern`,
      - `session/filename_pattern`,
      - `ui_last/import_jobcode`,
- [X] [GUI] Récupérer le statut import simple vs import avec copie : 
    - depuis `gtk_checkbox`,
    - vers `anselrc` -> `ui_last/import_copy`
- [X] [GUI] Récupérer la date du fichier :
    - depuis `gtk_calendar`,
    - vers _rien_ (on retourne chercher l'info dans le `gtk_label`),
    - [X] TODO: stocker dans `GDateTime ((dt_import_t *import)->date)`
    - [X] TODO: écrire une fonction `datetime.c:date_char_to_GDateTime()` (ou chercher si elle existe dans glib) pour écrire la date au format `GDateTime` dans `dt_import_t *import`, à partir du `char *` pris du `gtk_label`,

- [ ] Pour chaque fichier :
    - __Hypothèses__ :
      - le fichier a un type supporté (vérifié côté GUI dans `import.c`)
    - [ ] [I/O] : Charger fichier image dans un objet `dt_image_t *img` :
      - fichier vers `dt_cache_entry_t` avec `cache.c:dt_cache_entry_t *dt_cache_get_with_caller()`,
      - accéder à via `dt_image_t *img = (dt_image_t *)entry->data`
      - TODO: comment, bordel, lit-on le contenu de l'image ???
    - [X] [I/O] Récupérer les exifs : `exif.cc:dt_exif_read()`
      - entrée : `dt_image_t *img`,
      - sortie : EXIF mis à jour dans la structure `dt_image_t *img`
    - [ ] [I/O]: Récupérer le XMP : `exif.cc:dt_exif_xmp_read()`
      - entrée : `dt_image_t *img`,
      - sortie : EXIF mis à jour dans la structure `dt_image_t *img`
    - [ ] [I/O]: Récupérer les mémos audio et .txt
      - entrée : chemin d'origine
    - [X] [I/O]: Récupérer l'index incrémentable de l'image dans la séquence.
    - [X] [str] Expandre les variables dans le chemin de destination à partir des metadata
      - TODO
    - [X] [DB-I/O] Créer/récupérer un filmroll pour la liste de fichiers
      - Chercher si le répertoire est connu en BdD :
        - oui -> récupérer le `film_id`,
        - non -> créer filmroll en BdD et récuperer `film_id`,
    - [ ] [DB] :
      - Si pas copie : Chercher en BdD si un imgid existe pour le chemin du fichier __source__,
      - Si copie : Chercher en BdD si un imgid existe pour le chemin du fichier __destination__,
      - Dans tous les cas :
        - oui : récupérer `film_id` du filmroll via ??
        - non : créer nouveau filmroll
    - [X] [I/O] Si copie, copier chemin origine -> chemin destination
    - [ ] [DB] Mettre à jour l'entrée en BdB via `image_cache.c:dt_image_cache_write_release()`.