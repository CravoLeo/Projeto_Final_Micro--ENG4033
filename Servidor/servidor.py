from pymongo import MongoClient, ASCENDING, DESCENDING
from serial import Serial
from datetime import datetime
from time import time, sleep
from flask import Flask, render_template, request, redirect
import threading 

app = Flask(__name__)

serial_lock = threading.Lock()

cliente = MongoClient("localhost", 27017)
banco = cliente["cadastros"]
alunos = banco["alunos"]
presencas = banco["presencas"]


@app.route("/")
def pagInicial():
    busca = {}
    ordenCad = [["nome", ASCENDING]]
    ordenPres = [["horario_chegada", ASCENDING]]
    cadastros = list(alunos.find(busca, sort=ordenCad))
    lista_presenca = list(presencas.find(busca, sort=ordenPres))

    nomeRFID = {cad["rfid"]: cad["nome"] for cad in cadastros}

    return render_template("bancoDados.html", cadastros=cadastros, presencas=lista_presenca, nomeRFID=nomeRFID)

@app.route("/adicionar_aluno")
def adicionar_aluno():
    return render_template("formularioAluno.html")

@app.route("/adicionar_info", methods=['POST'])
def adicionar_info():
    nome = request.form.get("nome")
    sobrenome = request.form.get("sobrenome")
    rfid = request.form.get("RFID")
    
    nome_completo = nome + ' ' + sobrenome
    documento = {"nome": nome_completo, "rfid": rfid}
    alunos.insert_one(documento)
    
    return f"""
        <p>Aluno {nome} adicionado com sucesso!</p>
        
        <script>
            alert("Aluno {nome} adicionado com sucesso!");
            window.location.href = "/";
        </script>
    """

@app.route("/remover_aluno/<rfid>")
def remover_aluno(rfid):
    alunos.delete_one({"rfid": rfid})
    return """
        <p>Cadastro removido com sucesso</p>
        
        <script>
            alert("Cadastro removido com sucesso");
            window.location.href = "/";
        </script>
    """

@app.route("/remover_presenca")
def remover_presenca():
    presencas.delete_many({})
    return """
        <p>Presenças removidas com sucesso</p>
        
        <script>
            alert("Presenças removidas com sucesso");
            window.location.href = "/";
        </script>
    """

@app.route("/horario/<rfid>")
def horarioAluno(rfid):
    return render_template("formularioHorario.html", rfid=rfid)

@app.route("/adicionar_horario/<rfid>", methods=['GET', 'POST'])
def adicionar_horario(rfid):
    diaSemana = ["seg", "ter", "qua", "qui", "sex", "sab", "dom"]
    dia = request.form.get("dia")
    horario_inicio = request.form.get("horario_inicio")
    horario_final = request.form.get("horario_final")
    
    novo_horario = {"dia": dia, "horario_inicio": horario_inicio, "horario_final": horario_final}
    
    aluno = alunos.find_one({"rfid": rfid})
    horarios = aluno.get("horarios", [])
    
    horarios.append(novo_horario)
    horarios.sort(key=lambda h: (diaSemana.index(h["dia"]), h["horario_inicio"], h["horario_final"]))
    
    
    alunos.update_one({"rfid": rfid}, {"$set":{"horarios":horarios}})
    mensagem = f"Horario {horario_inicio}-{horario_final} em {dia} adicionado com sucesso para {rfid}"
    
    return redirect("/")

@app.route("/remover_horario", methods=['POST'])
def remover_horario():
    rfid = request.form.get("rfid")
    dia_da_semana = request.form.get("dia")
    horario_inicio = request.form.get("horario_inicio")
    horario_final = request.form.get("horario_final")
    
    horario = {"dia": dia_da_semana, "horario_inicio": horario_inicio, "horario_final": horario_final}
    
    alunos.update_one({"rfid": rfid}, {"$pull": {"horarios": horario}})

    return redirect("/")

@app.route("/horariosAluno/<rfid>")
def horarios_alunos(rfid):
    busca = {"rfid": rfid}
    hora_aluno = alunos.find_one(busca)
    
    return render_template("horariosAluno.html", aluno=hora_aluno)

@app.route("/sincronizar")
def sincronizar():
    busca = {}
    ordenacao = [["nome", ASCENDING]]
    cadastros = list(alunos.find(busca, sort=ordenacao))

    for cad in cadastros:
        rfid = str(cad["rfid"])
        nome = cad["nome"]
        horarios = cad.get("horarios") or []

        print(f"[SYNC] {nome} ({rfid}) - {len(horarios)} horarios")

        for hora in horarios:
            dia = hora["dia"]
            inicio = hora["horario_inicio"]
            final = hora["horario_final"]
            aluno = f"Dado:{rfid},{nome},{dia},{inicio},{final}\n"
            print("ENVIANDO:", aluno.strip()) 

            with serial_lock:
                meu_serial.write(aluno.encode("UTF-8"))
                sleep(0.1)

                inicio_espera = time()
                while True:
                    recebido = meu_serial.readline().decode().strip()
                    if recebido == "OK":
                        print("Arduino confirmou processamento.")
                        break
                    elif recebido == "ERRO":
                        print("Arduino retornou ERRO para:", aluno.strip())
                        break
                    elif recebido != "":
                        print("Arduino:", recebido)

                    # se passar de 3 segundos, desisto
                    if time() - inicio_espera > 3:
                        print("TIMEOUT esperando resposta do Arduino para:", aluno.strip())
                        break

                sleep(0.05)
        
    agora = datetime.now()
    hora = agora.strftime("%H:%M:%S")
    horario = f"hora:{hora}\n"
    meu_serial.write(horario.encode("UTF-8"))
    
    return """
        <script>
            alert("Sincronização concluída (veja o terminal para detalhes).");
            window.location.href = "/";
        </script>
    """


meu_serial = Serial("COM38", baudrate=9600, timeout=1)

def leitura_serial():
    while True:
        with serial_lock:
            recebido = meu_serial.readline().decode().strip()
        if recebido != "":
            print("Arduino:", recebido)
            if "PR_ALL:" in recebido:
                recebido = recebido.replace("PR_ALL:", "")
                serial_presenca = recebido.split("|")
                for pre in serial_presenca:
                    if not pre.strip():
                        continue
                    posVir = pre.find(",")
                    rfid = pre[:posVir]
                    horario_completo = pre[posVir+1:]
                    posVirSep = horario_completo.find(",")
                    
                    hora = horario_completo[:posVirSep]
                    minuto = horario_completo[posVirSep+1:]
                    horarioChegada = f"{hora}:{minuto}"
                    
                    presenca = {
                        "rfid": rfid,
                        "horario_chegada": horarioChegada,
                    }
                    presencas.insert_one(presenca)
                    print(f"Presença cadastrada")

        sleep(0.2)


threading.Thread(target=leitura_serial, daemon=True).start()



app.run(port=5004)
