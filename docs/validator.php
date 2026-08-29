<?php
/*
 * REFERENCE COPY ONLY - this file is not deployed from this repository.
 * It lives on the rink's server at:
 *   /logic/turniket_api/validator.php
 *
 * Kept here so firmware changes can be reasoned about without server access.
 * It may drift from the deployed version - check before relying on it.
 *
 * Note: the deployed version reads the API key from the "Hash" header. An
 * earlier revision used "Authorization"; the copy below still shows that.
 */

include "./config.php";
include "../../db.php";

// reject anything that is not POST
if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(["status" => "error", "action" => "DENY", "message" => "Metoda neni povolena."]);
    exit();
}

// turnstile authentication (API key in the header)
$headers = getallheaders();
$api_key = $headers['Authorization'];

if ($api_key !== $platny_api_key) {
    http_response_code(401);
    echo json_encode(["status" => "error", "action" => "DENY", "message" => "Neautorizovany pristup turniketu."]);
    exit();
}

// read and decode the turnstile's payload
$input = json_decode(file_get_contents("php://input"), true);
$qr_data = $input['qr_code'];

if (empty($qr_data)) {
    http_response_code(400);
    echo json_encode(["status" => "error", "action" => "DENY", "message" => "Chybi data QR kodu."]);
    exit();
}

// validate the QR against the database
$sql = $db->prepare("select v.*, b.*, count(*) as pocet from vstupenka v join blok b on v.blok_id=b.ID where qr_data = ? limit 1;");
$sql->execute(array($qr_data));
$vstupenka = $sql->fetch();

// is the ticket in the database at all?
if ($vstupenka['pocet'] != 1){
    http_response_code(200);
    echo json_encode([
        "status" => "not_found",
        "action" => "DENY",
        "message" => "Vstupenka nenalezena"
    ]);
    exit();
}

// check validity window
$duvod = "";
$now = time();

// entry allowed from 15 minutes before the start until the end
$zacatek = $vstupenka['datum_start'] - (15 * 60);
$konec = $vstupenka['datum_konec'];

if ($now < $zacatek or $now > $konec){
    $duvod .= "Vstup je mozny 15 minut pred zacatkem a maximalne do konce platnosti vstupenky";
}

if ($duvod != ""){
    http_response_code(200);
    echo json_encode([
        "status" => "invalid",
        "action" => "DENY",
        "message" => "Neplatna vstupenka - $duvod"
    ]);
    exit();
} else {
    // individual session types get a single entry
    if ($vstupenka['type'] == "Veřejné bruslení" or $vstupenka['type'] == "Bruslení pro nejmenší" or $vstupenka['type'] == "Hokej pro všechny" or $vstupenka['type'] == "Hokej rodiče a děti"){
        http_response_code(200);
        echo json_encode([
            "status" => "valid",
            "action" => "ALLOW_SINGLE",
            "message" => "Vstupenka je platna."
        ]);
    } else {
        // group ticket - the turnstile is held open longer
        http_response_code(200);
        echo json_encode([
            "status" => "valid",
            "action" => "ALLOW_MULTI",
            "message" => "Vstupenka je platna."
        ]);
    }

    // mark the ticket as scanned
    $nactena = $vstupenka['nactena'] + 1;
    $upd = $db->prepare("update vstupenka set nactena = ? where qr_data = ? limit 1");
    $upd->execute(array($nactena, $qr_data));
    exit();
}
?>
