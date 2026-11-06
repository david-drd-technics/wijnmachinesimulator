package be.technodream.wijnmachine;

import com.fazecast.jSerialComm.SerialPort;
import javax.swing.*;
import javax.swing.table.DefaultTableModel;
import java.awt.*;
import java.awt.event.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.regex.*;

public class WijnmachineSimulator extends JFrame {
    private SerialPort serial;
    private volatile boolean connected;
    private volatile boolean simulation=false;
    private final JComboBox<String> ports=new JComboBox<>();
    private final JLabel status=new JLabel("Niet verbonden"), mode=new JLabel("Modus onbekend");
    private final JTextArea raw=new JTextArea(8,72);
    private final DefaultTableModel logModel=new DefaultTableModel(new String[]{"Tijd","Richting","Bericht"},0) {
        public boolean isCellEditable(int r,int c){return false;}
    };
    private final JTable log=new JTable(logModel);
    private final JLabel[] counters=new JLabel[15], outs=new JLabel[15], motorState=new JLabel[3];
    private final JTextField[] steps=new JTextField[3], intervals=new JTextField[3];
    private final JToggleButton[] sensors=new JToggleButton[3];
    private final JPanel drawing=new MachineDrawing();
    private final DateTimeFormatter clock=DateTimeFormatter.ofPattern("HH:mm:ss.SSS");
    private final JTextField custom=new JTextField(34), uid=new JTextField("A1:B2:C3:D4",18);
    private final int[] motorSteps={1000,800,500}, motorInterval={1000,1200,2400};
    private final Pattern counterItem=Pattern.compile("\\{\"fles\":\"fles(\\d+)\",\"waarde\":(\\d+)\\}");
    private boolean pendingPhysicalConfirmation=false;

    public static void main(String[] args){
        SwingUtilities.invokeLater(()->new WijnmachineSimulator().setVisible(true));
    }
    public WijnmachineSimulator(){
        super("Wijnmachine - handmatige simulator (Arduino Mega 2560)");
        setDefaultCloseOperation(DO_NOTHING_ON_CLOSE);
        addWindowListener(new WindowAdapter(){public void windowClosing(WindowEvent e){disconnect();dispose();}});
        setSize(1320,940); setMinimumSize(new Dimension(950,700));
        setLocationRelativeTo(null);
        JPanel root=new JPanel(new BorderLayout(8,8));
        root.setBorder(BorderFactory.createEmptyBorder(10,10,10,10));
        setContentPane(root);
        root.add(connectionPanel(),BorderLayout.NORTH);
        JTabbedPane tabs=new JTabbedPane();
        tabs.addTab("Machine & motoren",motorPanel());
        tabs.addTab("Flessen & reset",bottlePanel());
        tabs.addTab("15 uitgangen",outputPanel());
        tabs.addTab("RFID & knoppen",inputPanel());
        tabs.addTab("TX/RX monitor",logPanel());
        root.add(tabs,BorderLayout.CENTER);
        refreshPorts();
    }
    private JPanel panel(String title){
        JPanel p=new JPanel();
        p.setLayout(new BoxLayout(p,BoxLayout.Y_AXIS));
        p.setBorder(BorderFactory.createTitledBorder(title));
        return p;
    }
    private JButton button(String name,Runnable task){
        JButton b=new JButton(name);
        b.addActionListener(e->task.run());
        return b;
    }
    private void addRow(JPanel p,Component... cs){
        JPanel r=new JPanel(new FlowLayout(FlowLayout.LEFT,8,5));
        for(Component c:cs)r.add(c);
        r.setAlignmentX(Component.LEFT_ALIGNMENT);p.add(r);
    }
    private JPanel connectionPanel(){
        JPanel p=panel("USB / COM verbinding");
        JButton refresh=button("Poorten vernieuwen",this::refreshPorts);
        JButton connect=button("Verbinden",()->{
            if(connected)disconnect();else connect();
        });
        connect.setName("connect");
        addRow(p,new JLabel("COM-poort:"),ports,refresh,connect,status);
        addRow(p,button("GET_STATUS",()->send("GET_STATUS")),
            button("Simulatie AAN",()->send("SIM_MODE_ON")),
            button("Simulatie UIT",()->{
                if(JOptionPane.showConfirmDialog(this,
                  "SIM_MODE_OFF schakelt over op ECHTE hardware. Doorgaan?",
                  "Hardwaremodus",JOptionPane.YES_NO_OPTION,JOptionPane.WARNING_MESSAGE)==JOptionPane.YES_OPTION)
                    send("SIM_MODE_OFF");
            }),
            mode,button("STOP_ALL",()->send("STOP_ALL")));
        return p;
    }
    private JPanel motorPanel(){
        JPanel p=new JPanel(new BorderLayout(8,8));
        drawing.setPreferredSize(new Dimension(960,255));
        drawing.setBorder(BorderFactory.createTitledBorder("Schematische machineweergave (geen gemeten positie)"));
        p.add(drawing,BorderLayout.NORTH);
        JPanel controls=new JPanel();
        controls.setLayout(new BoxLayout(controls,BoxLayout.Y_AXIS));
        String[] names={"Transportband","Deur","Glaslift"};
        String[][] labels={{"VOORUIT","ACHTERUIT"},{"OPEN","SLUIT"},{"OMHOOG","OMLAAG"}};
        for(int i=0;i<3;i++){
            final int n=i;
            JPanel group=panel("Motor "+(i+1)+" - "+names[i]);
            steps[i]=new JTextField(""+motorSteps[i],7);
            intervals[i]=new JTextField(""+motorInterval[i],7);
            sensors[i]=new JToggleButton("Virtuele HOME sensor UIT");
            sensors[i].addActionListener(e->{
                boolean on=sensors[n].isSelected();
                sensors[n].setText("Virtuele HOME sensor "+(on?"AAN":"UIT"));
                send("SIM_HOME_"+(n+1)+(on?"_ON":"_OFF"));
            });
            motorState[i]=new JLabel("Status: onbekend");
            addRow(group,new JLabel("Stappen:"),steps[i],new JLabel("Interval (us):"),intervals[i],
                button(labels[i][0],()->move(n,false)),button(labels[i][1],()->move(n,true)),
                button("HOME",()->send("MOTOR_"+(n+1)+"_HOME")),
                button("STOP",()->send("MOTOR_"+(n+1)+"_STOP")),
                sensors[i],motorState[i]);
            controls.add(group);
        }
        JScrollPane s=new JScrollPane(controls);
        p.add(s,BorderLayout.CENTER);
        return p;
    }
    private void move(int index,boolean reverse){
        long st,us;
        try{
            st=Long.parseLong(steps[index].getText().trim());
            us=Long.parseLong(intervals[index].getText().trim());
            if(st<1||st>1000000||us<100||us>50000)throw new NumberFormatException();
        }catch(NumberFormatException ex){
            JOptionPane.showMessageDialog(this,"Gebruik 1-1.000.000 stappen en interval 100-50.000 us.");
            return;
        }
        if(!simulation){
            if(JOptionPane.showConfirmDialog(this,
                 "De echte motor "+(index+1)+" zal "+st+" stappen bewegen. Zorg dat de machine veilig staat.",
                 "Echte motorbeweging",JOptionPane.YES_NO_OPTION,JOptionPane.WARNING_MESSAGE)
                 !=JOptionPane.YES_OPTION)return;
        }
        send("MOTOR_"+(index+1)+"_MOVE_"+(reverse?"REV":"FWD")+"_"+st+"_"+us);
    }
    private JPanel bottlePanel(){
        JPanel p=new JPanel(new GridLayout(0,3,8,8));
        for(int i=0;i<15;i++){
            final int n=i;
            JPanel b=panel("Fles "+(i+1));
            counters[i]=new JLabel("Teller: ?");
            JButton reset=button("Reset teller",()->send("RESET_COUNTER_"+(n+1)));
            JButton pulse=button("Virtuele resetpuls",()->{
                send("SIM_RESET_"+(n+1)+"_PRESS");
                javax.swing.Timer t=new javax.swing.Timer(150,e->send("SIM_RESET_"+(n+1)+"_RELEASE"));
                t.setRepeats(false);t.start();
            });
            addRow(b,counters[i]);addRow(b,reset,pulse);p.add(b);
        }
        return new JPanel(new BorderLayout()){{add(new JScrollPane(p),BorderLayout.CENTER);}};
    }
    private JPanel outputPanel(){
        JPanel p=new JPanel(new GridLayout(0,3,8,8));
        for(int i=0;i<15;i++){
            final int n=i;
            JPanel g=panel("Uitgang "+(i+1)+" / A"+i);
            outs[i]=new JLabel("Onbekend");
            addRow(g,outs[i],button("AAN",()->send("OUTPUT_"+(n+1)+"_ON")),
                button("UIT",()->send("OUTPUT_"+(n+1)+"_OFF")),
                button("WISSEL",()->send("OUTPUT_"+(n+1)+"_TOGGLE")));
            p.add(g);
        }
        return new JPanel(new BorderLayout()){{add(new JScrollPane(p),BorderLayout.CENTER);}};
    }
    private JPanel inputPanel(){
        JPanel p=panel("Virtuele bedieningsknoppen & RFID");
        for(int i=1;i<=3;i++){
            final int n=i;
            addRow(p,new JLabel("Knop "+i),
                button("INDRUKKEN",()->send("SIM_BUTTON_"+n+"_PRESS")),
                button("LOSLATEN",()->send("SIM_BUTTON_"+n+"_RELEASE")));
        }
        addRow(p,new JLabel("RFID UID"),uid,button("SIMULEER KAART",()->send("SIM_CARD_"+uid.getText().trim())));
        addRow(p,new JLabel("Eigen seriele opdracht"),custom,button("VERSTUUR",()->send(custom.getText().trim())));
        addRow(p,new JLabel("Simulatiecommando's vereisen SIM_MODE_ON."));
        return p;
    }
    private JPanel logPanel(){
        JPanel p=new JPanel(new BorderLayout(4,4));
        log.setAutoCreateRowSorter(true);
        log.getColumnModel().getColumn(0).setPreferredWidth(110);
        log.getColumnModel().getColumn(1).setPreferredWidth(70);
        log.getColumnModel().getColumn(2).setPreferredWidth(1050);
        p.add(new JScrollPane(log),BorderLayout.CENTER);
        JPanel bottom=new JPanel(new BorderLayout());
        raw.setEditable(false);
        raw.setFont(new Font(Font.MONOSPACED,Font.PLAIN,12));
        bottom.add(new JScrollPane(raw),BorderLayout.CENTER);
        JPanel actions=new JPanel(new FlowLayout(FlowLayout.LEFT));
        actions.add(button("Log wissen",()->{logModel.setRowCount(0);raw.setText("");}));
        actions.add(button("Log opslaan",this::exportLog));
        actions.add(new JLabel("TX = verzonden, RX = ontvangen, SYS = systeem"));
        bottom.add(actions,BorderLayout.SOUTH);
        bottom.setPreferredSize(new Dimension(500,200));
        p.add(bottom,BorderLayout.SOUTH);
        return p;
    }
    private void exportLog(){
        JFileChooser chooser=new JFileChooser();
        chooser.setSelectedFile(new File("wijnmachine-log.csv"));
        if(chooser.showSaveDialog(this)!=JFileChooser.APPROVE_OPTION)return;
        try(PrintWriter w=new PrintWriter(chooser.getSelectedFile(),StandardCharsets.UTF_8)){
            w.println("tijd;richting;bericht");
            for(int r=0;r<logModel.getRowCount();r++){
                for(int c=0;c<3;c++){
                    if(c>0)w.print(';');
                    w.print('"');
                    w.print(String.valueOf(logModel.getValueAt(r,c)).replace("\"","\"\""));
                    w.print('"');
                }
                w.println();
            }
            logMessage("SYS","Log opgeslagen");
        }catch(Exception ex){JOptionPane.showMessageDialog(this,ex.getMessage());}
    }
    private void refreshPorts(){
        ports.removeAllItems();
        for(SerialPort p:SerialPort.getCommPorts())ports.addItem(p.getSystemPortName());
    }
    private void connect(){
        String name=(String)ports.getSelectedItem();
        if(name==null){logMessage("ERR","Geen COM-poort gevonden");return;}
        SerialPort port=SerialPort.getCommPort(name);
        port.setComPortParameters(9600,8,1,SerialPort.NO_PARITY);
        port.setComPortTimeouts(SerialPort.TIMEOUT_NONBLOCKING,0,0);
        if(!port.openPort()){logMessage("ERR","Kan "+name+" niet openen. Sluit Arduino Serial Monitor.");return;}
        serial=port;connected=true;simulation=false;
        status.setText("Verbonden: "+name+" @ 9600");mode.setText("Modus onbekend");
        logMessage("SYS","Verbinding geopend: "+name);
        for(Component c:((JPanel)getContentPane().getComponent(0)).getComponents()){
            if(c instanceof JPanel)for(Component b:((JPanel)c).getComponents())
                if(b instanceof JButton && "connect".equals(b.getName()))((JButton)b).setText("Verbreken");
        }
        Thread reader=new Thread(()->receiveLoop(port),"Arduino-serial-reader");
        reader.setDaemon(true);reader.start();
        javax.swing.Timer t=new javax.swing.Timer(2300,e->{if(connected)send("GET_STATUS");});
        t.setRepeats(false);t.start();
    }
    private void disconnect(){
        connected=false;
        SerialPort p=serial;serial=null;
        if(p!=null)p.closePort();
        status.setText("Niet verbonden");
        mode.setText("Modus onbekend");simulation=false;
        logMessage("SYS","Verbinding verbroken");
        SwingUtilities.invokeLater(()->{
            for(Component c:((JPanel)getContentPane().getComponent(0)).getComponents()){
                if(c instanceof JPanel)for(Component b:((JPanel)c).getComponents())
                    if(b instanceof JButton && "connect".equals(b.getName()))((JButton)b).setText("Verbinden");
            }
        });
    }
    private void send(String command){
        if(!connected || serial==null || !serial.isOpen()){
            logMessage("ERR","Niet verbonden: "+command);return;
        }
        if(command==null||command.isBlank()||command.contains("\n")||command.contains("\r"))return;
        byte[] data=(command+"\n").getBytes(StandardCharsets.UTF_8);
        synchronized(this){
            long written=serial.writeBytes(data,data.length);
            logMessage(written==data.length?"TX":"ERR",command);
        }
    }
    private void receiveLoop(SerialPort port){
        byte[] buffer=new byte[2048];StringBuilder line=new StringBuilder();
        while(connected && serial==port && port.isOpen()){
            int available=port.bytesAvailable();
            if(available<0){SwingUtilities.invokeLater(()->logMessage("ERR","COM-poort losgekoppeld"));break;}
            if(available==0){
                try{Thread.sleep(12);}catch(InterruptedException e){Thread.currentThread().interrupt();break;}
                continue;
            }
            int n=port.readBytes(buffer,Math.min(buffer.length,available));
            if(n<0){SwingUtilities.invokeLater(()->logMessage("ERR","Leesfout op COM-poort"));break;}
            for(int i=0;i<n;i++){
                char ch=(char)(buffer[i]&0xff);
                if(ch=='\n'){
                    String msg=line.toString().trim();line.setLength(0);
                    if(!msg.isEmpty())SwingUtilities.invokeLater(()->received(msg));
                }else if(ch!='\r'){
                    if(line.length()<20000)line.append(ch);
                    else line.setLength(0);
                }
            }
        }
        if(connected && serial==port)SwingUtilities.invokeLater(this::disconnect);
    }
    private void received(String json){
        logMessage("RX",json);
        if(json.contains("\"event\":\"simulation\"")){
            simulation=json.contains("\"status\":\"enabled\"");
            mode.setText(simulation?"SIMULATIE AAN (motoren virtueel)":"ECHTE HARDWARE");
        }
        Matcher m=counterItem.matcher(json);
        while(m.find()){
            int n=Integer.parseInt(m.group(1));
            if(n>=1&&n<=15)counters[n-1].setText("Teller: "+m.group(2));
        }
        if(json.contains("\"event\":\"counter\"")){
            Matcher n=Pattern.compile("\"fles\":\"fles(\\d+)\"").matcher(json);
            Matcher v=Pattern.compile("\"waarde\":(\\d+)").matcher(json);
            if(n.find()&&v.find()){
                int i=Integer.parseInt(n.group(1))-1;
                if(i>=0&&i<15)counters[i].setText("Teller: "+v.group(1));
            }
        }
        if(json.contains("\"event\":\"output\"")){
            Matcher n=Pattern.compile("\"output\":(\\d+)").matcher(json);
            Matcher v=Pattern.compile("\"state\":\"(ON|OFF)\"").matcher(json);
            if(n.find()&&v.find()){
                int i=Integer.parseInt(n.group(1))-1;
                if(i>=0&&i<15)outs[i].setText(v.group(1));
            }
        }
        if(json.contains("\"event\":\"output_status\"")){
            Matcher m2=Pattern.compile("\"values\":\\[([^]]*)\\]").matcher(json);
            if(m2.find()){
                String[] a=m2.group(1).split(",");
                for(int i=0;i<Math.min(15,a.length);i++)outs[i].setText(a[i].trim().equals("1")?"ON":"OFF");
            }
        }
        if(json.contains("\"event\":\"motor_status\"")){
            Matcher n=Pattern.compile("\"motor\":(\\d+)").matcher(json);
            Matcher v=Pattern.compile("\"status\":\"([^\"]+)\"").matcher(json);
            if(n.find()&&v.find()){
                int i=Integer.parseInt(n.group(1))-1;
                if(i>=0&&i<3)motorState[i].setText("Status: "+v.group(1));
            }
        }
        drawing.repaint();
    }
    private void logMessage(String direction,String message){
        if(!SwingUtilities.isEventDispatchThread()){
            SwingUtilities.invokeLater(()->logMessage(direction,message));return;
        }
        logModel.addRow(new Object[]{LocalTime.now().format(clock),direction,message});
        raw.append(direction+" | "+message+"\n");
        raw.setCaretPosition(raw.getDocument().getLength());
    }
    private class MachineDrawing extends JPanel {
        protected void paintComponent(Graphics g){
            super.paintComponent(g);
            Graphics2D q=(Graphics2D)g.create();
            q.setRenderingHint(RenderingHints.KEY_ANTIALIASING,RenderingHints.VALUE_ANTIALIAS_ON);
            int w=getWidth(),h=getHeight();
            q.setColor(new Color(20,34,49));q.fillRoundRect(12,22,w-24,h-32,16,16);
            q.setColor(Color.WHITE);q.setFont(new Font("SansSerif",Font.BOLD,13));
            q.drawString("TRANSPORTBAND / MOTOR 1",30,48);
            int usable=Math.max(160,w-300),pitch=Math.max(12,Math.min(34,usable/15));
            int beltW=pitch*15;
            q.setColor(new Color(100,119,142));q.fillRoundRect(28,133,beltW,13,7,7);
            for(int i=0;i<15;i++){
                int x=34+i*pitch;
                q.setColor(new Color(130,180,207));q.fillRoundRect(x,78,Math.max(8,pitch-8),50,5,5);
                q.setColor(new Color(202,225,242));q.fillRect(x+3,72,Math.max(3,pitch-14),8);
                q.setColor(Color.WHITE);q.setFont(new Font("SansSerif",Font.PLAIN,10));
                q.drawString(""+(i+1),x+2,164);
            }
            int doorX=Math.min(w-135,52+beltW);
            q.setColor(Color.WHITE);q.drawString("DEUR / MOTOR 2",doorX,48);
            q.setColor(new Color(64,108,142));q.fillRoundRect(doorX+10,75,43,75,5,5);
            int liftX=Math.min(w-62,doorX+75);
            q.setColor(Color.WHITE);q.drawString("LIFT",liftX,65);
            q.setColor(new Color(183,212,226));q.fillRect(liftX+8,84,5,65);
            q.fillRoundRect(liftX-2,132,33,5,4,4);
            q.setColor(new Color(101,173,207));q.fillRoundRect(liftX+4,116,22,16,4,4);
            q.dispose();
        }
    }
}
